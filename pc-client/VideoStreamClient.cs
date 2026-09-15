using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Channels;

namespace MagicTupperPcClient;

/// <summary>
/// Network-only Remote Play receiver.
/// Remote Play keeps UDP reception, H.264 decoding and PCM playback on independent paths so
/// Media Foundation or audio scheduling can never stall the video socket receive loop.
/// </summary>
internal sealed class VideoStreamClient : IDisposable
{
    public const int Port = 8768;

    private CancellationTokenSource? cts;
    private UdpClient? videoUdp;
    private UdpClient? audioUdp;
    private IPEndPoint? switchEndpoint;
    private MediaFoundationH264Decoder? decoder;
    private NetworkPcmAudioPlayer? audio;
    private readonly object audioGate = new();
    private volatile bool audioPlaybackEnabled = true;

    public int AudioDeviceNumber { get; set; } = -1;
    public float AudioVolume { get; set; } = 0.8f;
    public bool AudioEnabled { get; set; } = true;

    private uint? currentTimestamp;
    private readonly MemoryStream accessUnit = new(512 * 1024);
    private MemoryStream? fragmentedNal;
    private ushort? lastSeq;
    private bool waitingForIdr = true;
    private bool currentHasIdr;

    private long packets, frames, dropped, accessUnits, sps, pps, idr, nalUnits;
    private long decoderQueueDrops;
    private long audioPackets, audioBytes, audioGapFrames, audioLatePackets;
    private uint? expectedAudioTimestamp;
    private ushort? expectedAudioSequence;
    private readonly Dictionary<ushort, AudioRtpPacket> audioJitter = new();
    private readonly object audioJitterGate = new();
    private long audioReorderedPackets, audioSkippedPackets;
    private sealed record AudioRtpPacket(ushort Sequence, uint Timestamp, byte[] Pcm);
    private long lastTelemetryTick, lastTelemetryFrames;
    private long lastTelemetryAudioBytes, lastTelemetryPlayerEnqueued, lastTelemetryPlayerTrimmed;
    private int lastTelemetryPlayerBuffered;
    private int cleanupGuard;
    private uint? homeJpegFrameId;
    private byte[]? homeJpegBuffer;
    private bool[]? homeJpegChunks;
    private int homeJpegChunksReceived;
    private const int HomeJpegChunkPayload = 1200 - 16;

    public event Action<string>? Status;
    public event Action<Bitmap>? FrameReady;
    private long lastH264FrameMs; // prefer H.264 while game frames are flowing
    private uint latestHomeJpegFrameId; // rc7.9: reject late UDP fragments from superseded JPEG frames
    public event Action<bool>? RunningChanged;
    public bool Running => cts is not null;

    private sealed record EncodedAccessUnit(byte[] Data);
    private sealed record HomeJpegFrame(uint FrameId, byte[] Data);

    public void SetAudioVolume(float value)
    {
        AudioVolume = Math.Clamp(value, 0f, 1f);
        lock (audioGate) audio?.SetVolume(AudioVolume);
    }

    public void SetAudioDeviceNumber(int deviceNumber)
    {
        AudioDeviceNumber = deviceNumber;
        lock (audioGate)
        {
            if (!Running || audio is null) return;
            try { audio.Dispose(); } catch { }
            var replacement = new NetworkPcmAudioPlayer { DeviceNumber = AudioDeviceNumber, Volume = AudioVolume };
            replacement.Status += message => Status?.Invoke(message);
            replacement.Start();
            if (!audioPlaybackEnabled) replacement.FlushAndPause();
            audio = replacement;
            Status?.Invoke("AUDIO: salida cambiada · resincronizando");
        }
    }

    /// <summary>
    /// Runtime playback mute/unmute. This is deliberately local and immediate: disabling the
    /// checkbox must stop what the user hears even while the Switch stream remains active.
    /// Keeping the RTP receiver alive also lets us re-enable without restarting video.
    /// </summary>
    public void SetAudioPlaybackEnabled(bool enabled)
    {
        audioPlaybackEnabled = enabled;
        lock (audioGate)
        {
            if (!enabled)
            {
                audio?.FlushAndPause();
                lock (audioJitterGate)
                {
                    expectedAudioTimestamp = null;
                    expectedAudioSequence = null;
                    audioJitter.Clear();
                }
                Status?.Invoke("AUDIO: desactivado · reproducción silenciada");
                return;
            }

            if (audio is not null)
            {
                audio.Resume();
                lock (audioJitterGate)
                {
                    expectedAudioTimestamp = null;
                    expectedAudioSequence = null;
                    audioJitter.Clear();
                }
                Status?.Invoke("AUDIO: activado · esperando audio actual");
            }
        }
    }

    public void Start(string host)
    {
        if (Running) return;
        if (!IPAddress.TryParse(host, out var ip))
            throw new ArgumentException("IP de Switch no válida.");

        ResetCounters();
        cleanupGuard = 0;
        cts = new CancellationTokenSource();
        RunningChanged?.Invoke(true);
        var token = cts.Token;

        _ = Task.Run(async () =>
        {
            var decodeQueue = Channel.CreateBounded<EncodedAccessUnit>(new BoundedChannelOptions(3)
            {
                SingleReader = false, // receive loop may evict one stale AU when the queue is full
                SingleWriter = true,
                FullMode = BoundedChannelFullMode.Wait,
                AllowSynchronousContinuations = false
            });

            var homeDecodeQueue = Channel.CreateBounded<HomeJpegFrame>(new BoundedChannelOptions(1)
            {
                SingleReader = true,
                SingleWriter = true,
                FullMode = BoundedChannelFullMode.DropOldest,
                AllowSynchronousContinuations = false
            });

            Task? decodeTask = null;
            Task? homeDecodeTask = null;
            Task? audioTask = null;

            try
            {
                decoder = new MediaFoundationH264Decoder(1280, 720);
                decoder.Status += message => Status?.Invoke(message);

                audio = AudioEnabled
                    ? new NetworkPcmAudioPlayer { DeviceNumber = AudioDeviceNumber, Volume = AudioVolume }
                    : null;
                if (audio is not null)
                {
                    audio.Status += message => Status?.Invoke(message);
                    try
                    {
                        audio.Start();
                    }
                    catch (Exception ex)
                    {
                        Status?.Invoke($"AUDIO: desactivado por error local · {ex.Message}");
                        try { audio.Dispose(); } catch { }
                        audio = null;
                    }
                }

                videoUdp = new UdpClient(ip.AddressFamily);
                videoUdp.Client.ReceiveBufferSize = 8 * 1024 * 1024;
                videoUdp.Client.Bind(ip.AddressFamily == AddressFamily.InterNetworkV6
                    ? new IPEndPoint(IPAddress.IPv6Any, 0)
                    : new IPEndPoint(IPAddress.Any, 0));
                switchEndpoint = new IPEndPoint(ip, Port);

                int audioPort = 0;
                if (AudioEnabled)
                {
                    audioUdp = new UdpClient(ip.AddressFamily);
                    audioUdp.Client.ReceiveBufferSize = 2 * 1024 * 1024;
                    audioUdp.Client.Bind(ip.AddressFamily == AddressFamily.InterNetworkV6
                        ? new IPEndPoint(IPAddress.IPv6Any, 0)
                        : new IPEndPoint(IPAddress.Any, 0));
                    audioPort = ((IPEndPoint)audioUdp.Client.LocalEndPoint!).Port;
                    // Prime Windows' stateful UDP firewall/NAT path. The Switch sends audio
                    // from a fixed UDP/8769 source port back to this ephemeral local port.
                    try
                    {
                        byte[] prime = Encoding.ASCII.GetBytes("MTV2|AUDIO|HELLO");
                        await audioUdp.SendAsync(prime, new IPEndPoint(ip, 8769), token);
                    }
                    catch when (!token.IsCancellationRequested) { }
                    audioTask = Task.Run(() => AudioReceiveLoopAsync(ip, token), token);
                }

                decodeTask = Task.Run(() => DecodeLoopAsync(decodeQueue.Reader, token), token);
                homeDecodeTask = Task.Run(() => HomeJpegDecodeLoopAsync(homeDecodeQueue.Reader, token), token);

                Status?.Invoke($"VÍDEO: conectando con {host}:{Port}/UDP…");
                await SendControlAsync(
                    AudioEnabled
                        ? $"MTV2|START|AUDIO=1|APORT={audioPort}"
                        : "MTV2|START|AUDIO=0",
                    token);

                bool helloOk = false;
                long lastRx = Environment.TickCount64;
                long lastPing = 0;
                long lastStartRetry = Environment.TickCount64 - 500;
                long lastWaitingStatus = 0;

                while (!token.IsCancellationRequested)
                {
                    var now = Environment.TickCount64;
                    if (now - lastPing >= 1000)
                    {
                        lastPing = now;
                        try { await SendControlAsync("MTV2|PING", token); }
                        catch when (!token.IsCancellationRequested) { }
                    }

                    UdpReceiveResult r;
                    using (var receiveTimeout = CancellationTokenSource.CreateLinkedTokenSource(token))
                    {
                        receiveTimeout.CancelAfter(1200);
                        try
                        {
                            r = await videoUdp.ReceiveAsync(receiveTimeout.Token);
                        }
                        catch (OperationCanceledException) when (!token.IsCancellationRequested)
                        {
                            now = Environment.TickCount64;
                            if (now - lastRx >= 2500)
                            {
                                Status?.Invoke(helloOk
                                    ? "VÍDEO: SIN RESPUESTA · reintentando conexión…"
                                    : "VÍDEO: ESPERANDO RESPUESTA DEL SYSMODULE…");

                                if (!helloOk && now - lastStartRetry >= 500)
                                {
                                    lastStartRetry = now;
                                    try
                                    {
                                        await SendControlAsync(
                                            AudioEnabled
                                                ? $"MTV2|START|AUDIO=1|APORT={audioPort}"
                                                : "MTV2|START|AUDIO=0",
                                            token);
                                    }
                                    catch { }
                                }
                            }
                            continue;
                        }
                    }

                    // Dedicated video socket: only control + PT96 should arrive here.
                    if (!r.RemoteEndPoint.Address.Equals(ip))
                        continue;

                    lastRx = Environment.TickCount64;
                    var p = r.Buffer;

                    // Experimental HOME/system-UI JPEG fallback (MTJ1).
                    if (p.Length >= 16 &&
                        p[0] == (byte)'M' && p[1] == (byte)'T' &&
                        p[2] == (byte)'J' && p[3] == (byte)'1')
                    {
                        uint fid = BinaryPrimitives.ReadUInt32BigEndian(p.AsSpan(4, 4));
                        int off = checked((int)BinaryPrimitives.ReadUInt32BigEndian(p.AsSpan(8, 4)));
                        int total = checked((int)BinaryPrimitives.ReadUInt32BigEndian(p.AsSpan(12, 4)));
                        int count = p.Length - 16;
                        if (total > 0 && total <= 0x400000 && off >= 0 && off < total && count > 0 && off + count <= total)
                        {
                            // Latest-frame-wins: once a newer JPEG frame has started, never let a
                            // delayed UDP fragment resurrect an older incomplete frame.
                            if (latestHomeJpegFrameId != 0 && fid < latestHomeJpegFrameId)
                                continue;
                            if (fid > latestHomeJpegFrameId)
                                latestHomeJpegFrameId = fid;
                            int chunks = (total + HomeJpegChunkPayload - 1) / HomeJpegChunkPayload;
                            if (homeJpegFrameId != fid || homeJpegBuffer is null || homeJpegBuffer.Length != total)
                            {
                                homeJpegFrameId = fid;
                                homeJpegBuffer = new byte[total];
                                homeJpegChunks = new bool[chunks];
                                homeJpegChunksReceived = 0;
                            }

                            Buffer.BlockCopy(p, 16, homeJpegBuffer, off, count);
                            int ci = off / HomeJpegChunkPayload;
                            if (homeJpegChunks is not null && ci >= 0 && ci < homeJpegChunks.Length && !homeJpegChunks[ci])
                            {
                                homeJpegChunks[ci] = true;
                                homeJpegChunksReceived++;
                            }

                            if (homeJpegChunks is not null && homeJpegChunksReceived == homeJpegChunks.Length)
                            {
                                // rc7.10: never JPEG-decode on the UDP receive loop. Hand the complete
                                // frame to a capacity-1 latest-frame queue so network reception can
                                // immediately continue and stale complete JPEGs are dropped.
                                var completed = homeJpegBuffer;
                                homeDecodeQueue.Writer.TryWrite(new HomeJpegFrame(fid, completed));
                                homeJpegFrameId = null;
                                homeJpegBuffer = null;
                                homeJpegChunks = null;
                                homeJpegChunksReceived = 0;
                            }
                        }
                        continue;
                    }

                    if (p.Length >= 4 &&
                        p[0] == (byte)'M' && p[1] == (byte)'T' &&
                        p[2] == (byte)'V' && p[3] == (byte)'2')
                    {
                        var control = Encoding.ASCII.GetString(p);

                        if (control.StartsWith("MTV2|OK|", StringComparison.Ordinal))
                        {
                            helloOk = true;
                            Status?.Invoke("REMOTE PLAY: CONECTADO · vídeo y audio en sockets independientes");
                            lastTelemetryTick = Environment.TickCount64;
                            continue;
                        }

                        if (control.StartsWith("MTV2|HOME|CAPSSC|READY", StringComparison.Ordinal))
                        {
                            Status?.Invoke("HOME: caps:sc disponible · esperando captura del compositor…");
                            continue;
                        }

                        if (control.StartsWith("MTV2|HOME|CAPSSC|ERR|", StringComparison.Ordinal) ||
                            control.StartsWith("MTV2|HOME|CAPTURE|ERR|", StringComparison.Ordinal))
                        {
                            Status?.Invoke("HOME: probe error · " + control);
                            continue;
                        }

                        if (control.StartsWith("MTV2|WAIT|NO_GAME", StringComparison.Ordinal))
                        {
                            helloOk = true;
                            var tick = Environment.TickCount64;
                            if (tick - lastWaitingStatus >= 1000)
                            {
                                lastWaitingStatus = tick;
                                Status?.Invoke("VÍDEO: ESPERANDO · inicia un juego en la Switch");
                            }
                            continue;
                        }

                        if (control.StartsWith("MTV2|ERR|", StringComparison.Ordinal))
                        {
                            Status?.Invoke("VÍDEO: ERROR DEL SYSMODULE · " + control[9..]);
                            continue;
                        }

                        if (control.StartsWith("MTV2|AUDIO|THREAD|READY", StringComparison.Ordinal))
                        {
                            Status?.Invoke("AUDIO: hilo Switch activo · esperando primer bloque PCM");
                            continue;
                        }

                        if (control.StartsWith("MTV2|AUDIO|ACTIVE", StringComparison.Ordinal))
                        {
                            Status?.Invoke("AUDIO: captura grc:d activa · enviando PCM");
                            continue;
                        }

                        if (control.StartsWith("MTV2|AUDIO|ERR|", StringComparison.Ordinal))
                        {
                            Status?.Invoke("AUDIO: ERROR SYSMODULE · " + control["MTV2|AUDIO|ERR|".Length..]);
                            continue;
                        }

                        if (control.StartsWith("MTV2|AUDIO|DIAG|", StringComparison.Ordinal))
                        {
                            Status?.Invoke("AUDIO PROBE: " + control["MTV2|AUDIO|DIAG|".Length..]);
                            continue;
                        }

                        continue;
                    }

                    if (p.Length < 12 || (p[0] >> 6) != 2)
                        continue;

                    byte pt = (byte)(p[1] & 0x7F);
                    if (pt != 96)
                        continue;

                    helloOk = true;
                    Interlocked.Increment(ref packets);

                    ushort seq = BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(2, 2));
                    uint ts = BinaryPrimitives.ReadUInt32BigEndian(p.AsSpan(4, 4));
                    bool marker = (p[1] & 0x80) != 0;

                    bool gap = lastSeq.HasValue && (ushort)(lastSeq.Value + 1) != seq;
                    lastSeq = seq;
                    if (gap)
                    {
                        Interlocked.Increment(ref dropped);
                        waitingForIdr = true;
                        ResetAssembly();
                        currentTimestamp = ts;
                    }

                    if (currentTimestamp.HasValue && currentTimestamp.Value != ts)
                        ResetAssembly();
                    currentTimestamp = ts;

                    int payloadLength = p.Length - 12;
                    if (payloadLength <= 0)
                        continue;

                    const int payloadOffset = 12;
                    byte type = (byte)(p[payloadOffset] & 0x1F);
                    if (type == 7) Interlocked.Increment(ref sps);
                    else if (type == 8) Interlocked.Increment(ref pps);
                    else if (type == 5) Interlocked.Increment(ref idr);

                    if (type == 28)
                    {
                        if (payloadLength < 2)
                            continue;

                        bool start = (p[payloadOffset + 1] & 0x80) != 0;
                        bool end = (p[payloadOffset + 1] & 0x40) != 0;

                        if (start)
                        {
                            var fuType = (byte)(p[payloadOffset + 1] & 0x1F);
                            Interlocked.Increment(ref nalUnits);

                            if (fuType == 5)
                            {
                                currentHasIdr = true;
                                Interlocked.Increment(ref idr);
                            }
                            else if (fuType == 7) Interlocked.Increment(ref sps);
                            else if (fuType == 8) Interlocked.Increment(ref pps);

                            fragmentedNal?.Dispose();
                            fragmentedNal = new MemoryStream(payloadLength * 4);
                            WriteStartCode(fragmentedNal);
                            fragmentedNal.WriteByte((byte)((p[payloadOffset] & 0xE0) | fuType));
                            if (payloadLength > 2)
                                fragmentedNal.Write(p, payloadOffset + 2, payloadLength - 2);
                        }
                        else if (fragmentedNal is not null && payloadLength > 2)
                        {
                            fragmentedNal.Write(p, payloadOffset + 2, payloadLength - 2);
                        }

                        if (end && fragmentedNal is not null)
                        {
                            fragmentedNal.Position = 0;
                            fragmentedNal.CopyTo(accessUnit);
                            fragmentedNal.Dispose();
                            fragmentedNal = null;
                        }
                    }
                    else
                    {
                        Interlocked.Increment(ref nalUnits);
                        if (type == 5)
                            currentHasIdr = true;

                        WriteStartCode(accessUnit);
                        accessUnit.Write(p, payloadOffset, payloadLength);
                    }

                    EmitTelemetryIfDue();

                    if (!marker)
                        continue;

                    Interlocked.Increment(ref accessUnits);
                    var data = accessUnit.ToArray();
                    accessUnit.SetLength(0);
                    currentTimestamp = null;
                    fragmentedNal?.Dispose();
                    fragmentedNal = null;

                    if (waitingForIdr && !currentHasIdr)
                    {
                        currentHasIdr = false;
                        continue;
                    }

                    if (currentHasIdr)
                        waitingForIdr = false;
                    currentHasIdr = false;

                    // Never decode in the UDP receive loop. If the decoder/UI is momentarily late,
                    // keep real-time behavior by dropping an old AU rather than stopping reception.
                    var encoded = new EncodedAccessUnit(data);
                    if (!decodeQueue.Writer.TryWrite(encoded))
                    {
                        // Decoder/UI fell behind. Evict exactly one stale AU and keep the newest one.
                        decodeQueue.Reader.TryRead(out _);
                        Interlocked.Increment(ref decoderQueueDrops);
                        decodeQueue.Writer.TryWrite(encoded);
                    }
                }
            }
            catch (OperationCanceledException) { }
            catch (ObjectDisposedException) when (token.IsCancellationRequested) { }
            catch (SocketException ex) when (!token.IsCancellationRequested)
            {
                Status?.Invoke("VÍDEO: CONEXIÓN PERDIDA · " + ex.Message);
            }
            catch (Exception ex) when (!token.IsCancellationRequested)
            {
                Status?.Invoke("VÍDEO: ERROR · " + ex.Message);
            }
            finally
            {
                try { cts?.Cancel(); } catch { }
                decodeQueue.Writer.TryComplete();
                homeDecodeQueue.Writer.TryComplete();

                if (decodeTask is not null)
                {
                    try { await decodeTask; } catch { }
                }

                if (homeDecodeTask is not null)
                {
                    try { await homeDecodeTask; } catch { }
                }

                if (audioTask is not null)
                {
                    try { await audioTask; } catch { }
                }

                Cleanup();
            }
        }, token);
    }

    private async Task DecodeLoopAsync(ChannelReader<EncodedAccessUnit> reader, CancellationToken token)
    {
        await foreach (var au in reader.ReadAllAsync(token))
        {
            var d = decoder;
            if (d is null) break;

            Bitmap? bmp = null;
            try
            {
                bmp = d.DecodeAccessUnit(au.Data);
            }
            catch (Exception ex) when (!token.IsCancellationRequested)
            {
                Status?.Invoke("VÍDEO: decoder · " + ex.Message);
                waitingForIdr = true;
                continue;
            }

            if (bmp is null)
                continue;

            Interlocked.Increment(ref frames);
            Interlocked.Exchange(ref lastH264FrameMs, Environment.TickCount64);
            FrameReady?.Invoke(bmp);

            if (Interlocked.Read(ref frames) == 1)
                Status?.Invoke($"VÍDEO: ACTIVO · {d.Width}×{d.Height} · RTP/H.264 · recepción desacoplada");
        }
    }


    private async Task HomeJpegDecodeLoopAsync(ChannelReader<HomeJpegFrame> reader, CancellationToken token)
    {
        await foreach (var frame in reader.ReadAllAsync(token))
        {
            // H.264 always wins while game video is fresh. A HOME JPEG completed just
            // before the backend transition is discarded rather than displayed late.
            if (Environment.TickCount64 - Interlocked.Read(ref lastH264FrameMs) < 120)
                continue;
            try
            {
                using var ms = new MemoryStream(frame.Data, writable: false);
                using var img = Image.FromStream(ms, false, true);
                var bmp = new Bitmap(img);
                FrameReady?.Invoke(bmp);
                Status?.Invoke($"HOME: ACTIVO · {bmp.Width}×{bmp.Height} · JPEG/caps:sc · cadencia natural · latest-frame");
            }
            catch (Exception ex) when (!token.IsCancellationRequested)
            {
                Status?.Invoke("HOME: JPEG inválido · " + ex.Message);
            }
        }
    }

    private async Task AudioReceiveLoopAsync(IPAddress switchIp, CancellationToken token)
    {
        var u = audioUdp;
        if (u is null) return;

        while (!token.IsCancellationRequested)
        {
            UdpReceiveResult r;
            try { r = await u.ReceiveAsync(token); }
            catch (OperationCanceledException) { break; }
            catch (ObjectDisposedException) { break; }

            if (!r.RemoteEndPoint.Address.Equals(switchIp)) continue;
            var p = r.Buffer;
            if (p.Length >= 4 && p[0] == (byte)'M' && p[1] == (byte)'T' && p[2] == (byte)'V' && p[3] == (byte)'2')
            {
                var control = Encoding.ASCII.GetString(p);
                if (control.StartsWith("MTV2|AUDIO|ACTIVE", StringComparison.Ordinal)) Status?.Invoke("AUDIO: captura activa en Switch · PCM recibido por socket dedicado");
                else if (control.StartsWith("MTV2|AUDIO|ERR|", StringComparison.Ordinal)) Status?.Invoke("AUDIO: ERROR SYSMODULE · " + control["MTV2|AUDIO|ERR|".Length..]);
                continue;
            }
            if (p.Length < 12 || (p[0] >> 6) != 2 || (p[1] & 0x7F) != 97) continue;

            ushort seq = BinaryPrimitives.ReadUInt16BigEndian(p.AsSpan(2, 2));
            uint ts = BinaryPrimitives.ReadUInt32BigEndian(p.AsSpan(4, 4));
            int payloadLength = (p.Length - 12) & ~3;
            if (payloadLength <= 0) continue;
            Interlocked.Increment(ref audioPackets);
            Interlocked.Add(ref audioBytes, payloadLength);

            var readyPcm = new List<byte[]>(4);
            lock (audioJitterGate)
            {
                if (!expectedAudioSequence.HasValue) expectedAudioSequence = seq;
                else
                {
                    int deltaSeq = unchecked((short)(seq - expectedAudioSequence.Value));
                    if (deltaSeq < 0) { Interlocked.Increment(ref audioLatePackets); continue; }
                    if (deltaSeq > 0) Interlocked.Increment(ref audioReorderedPackets);
                }
                if (!audioJitter.ContainsKey(seq)) audioJitter[seq] = new AudioRtpPacket(seq, ts, p.AsSpan(12, payloadLength).ToArray());

                if (expectedAudioSequence.HasValue && !audioJitter.ContainsKey(expectedAudioSequence.Value) && audioJitter.Count >= 3)
                {
                    ushort expected = expectedAudioSequence.Value;
                    var next = audioJitter.Keys.Select(k => new { Seq = k, Distance = (ushort)(k - expected) }).Where(x => x.Distance > 0 && x.Distance < 0x8000).OrderBy(x => x.Distance).FirstOrDefault();
                    if (next is not null) { Interlocked.Add(ref audioSkippedPackets, next.Distance); expectedAudioSequence = next.Seq; expectedAudioTimestamp = null; }
                }

                while (expectedAudioSequence.HasValue && audioJitter.Remove(expectedAudioSequence.Value, out var ap))
                {
                    uint payloadFrames = (uint)(ap.Pcm.Length / 4);
                    if (expectedAudioTimestamp.HasValue)
                    {
                        int delta = unchecked((int)(ap.Timestamp - expectedAudioTimestamp.Value));
                        if (delta > 0 && delta <= 4800) Interlocked.Add(ref audioGapFrames, delta);
                        else if (delta < 0) { Interlocked.Increment(ref audioLatePackets); expectedAudioSequence++; continue; }
                    }
                    readyPcm.Add(ap.Pcm);
                    expectedAudioTimestamp = ap.Timestamp + payloadFrames;
                    expectedAudioSequence++;
                }
            }
            if (audioPlaybackEnabled && readyPcm.Count > 0)
            {
                lock (audioGate)
                    foreach (var pcm in readyPcm) audio?.Enqueue(pcm);
            }

        }
    }

    private void EmitTelemetryIfDue()
    {
        var tick = Environment.TickCount64;
        var previous = Interlocked.Read(ref lastTelemetryTick);
        if (previous == 0)
        {
            Interlocked.Exchange(ref lastTelemetryTick, tick);
            Interlocked.Exchange(ref lastTelemetryFrames, Interlocked.Read(ref frames));
            return;
        }

        if (tick - previous < 1000)
            return;

        if (Interlocked.CompareExchange(ref lastTelemetryTick, tick, previous) != previous)
            return;

        var elapsedMs = Math.Max(1, tick - previous);
        var f = Interlocked.Read(ref frames);
        var prevFrames = Interlocked.Exchange(ref lastTelemetryFrames, f);
        var fps = (f - prevFrames) * 1000.0 / elapsedMs;

        var rxBytesTotal = Interlocked.Read(ref audioBytes);
        var rxBytesDelta = rxBytesTotal - Interlocked.Exchange(ref lastTelemetryAudioBytes, rxBytesTotal);
        var rxBps = rxBytesDelta * 1000.0 / elapsedMs;

        long enqueuedTotal = audio?.EnqueuedBytes ?? 0;
        long trimmedTotal = audio?.TrimmedBytes ?? 0;
        int bufferedNow = audio?.BufferedBytes ?? 0;
        long enqueuedDelta = enqueuedTotal - Interlocked.Exchange(ref lastTelemetryPlayerEnqueued, enqueuedTotal);
        long trimmedDelta = trimmedTotal - Interlocked.Exchange(ref lastTelemetryPlayerTrimmed, trimmedTotal);
        int bufferedDelta = bufferedNow - Interlocked.Exchange(ref lastTelemetryPlayerBuffered, bufferedNow);
        // Approximate bytes consumed by the Windows output during this interval.
        // queued = previous + enqueued - trimmed - played  => played = enqueued - trimmed - delta(queue)
        long playedDelta = Math.Max(0, enqueuedDelta - trimmedDelta - bufferedDelta);
        var playedBps = playedDelta * 1000.0 / elapsedMs;

        Status?.Invoke(
            $"A/V: {fps:F1} FPS · vídeo RX {Interlocked.Read(ref packets)} " +
            $"loss {Interlocked.Read(ref dropped)} qdrop {Interlocked.Read(ref decoderQueueDrops)} · " +
            $"audio RX {Interlocked.Read(ref audioPackets)} gap {Interlocked.Read(ref audioGapFrames)} " +
            $"late {Interlocked.Read(ref audioLatePackets)} reorder {Interlocked.Read(ref audioReorderedPackets)} skip {Interlocked.Read(ref audioSkippedPackets)} · " +
            $"RX {rxBps/1000.0:F1}kB/s PLAY {playedBps/1000.0:F1}kB/s TRIM {trimmedDelta*1000.0/elapsedMs/1000.0:F1}kB/s · " +
            $"buffer {audio?.BufferedMilliseconds ?? 0} ms trims {audio?.TrimCount ?? 0} rebuf {audio?.RebufferCount ?? 0} · decode {f}");
    }

    private async Task SendControlAsync(string command, CancellationToken token)
    {
        var u = videoUdp ?? throw new InvalidOperationException("Socket de vídeo no inicializado.");
        var ep = switchEndpoint ?? throw new InvalidOperationException("Endpoint de Switch no inicializado.");
        var bytes = Encoding.ASCII.GetBytes(command);
        await u.SendAsync(bytes, ep, token);
    }

    private static void WriteStartCode(Stream s)
    {
        s.WriteByte(0);
        s.WriteByte(0);
        s.WriteByte(0);
        s.WriteByte(1);
    }

    private void ResetAssembly()
    {
        accessUnit.SetLength(0);
        fragmentedNal?.Dispose();
        fragmentedNal = null;
        currentTimestamp = null;
        currentHasIdr = false;
    }

    private void ResetCounters()
    {
        packets = frames = dropped = accessUnits = sps = pps = idr = nalUnits = 0;
        decoderQueueDrops = 0;
        audioPackets = audioBytes = audioGapFrames = audioLatePackets = 0;
        lock (audioJitterGate)
        {
            expectedAudioTimestamp = null;
            expectedAudioSequence = null;
            audioJitter.Clear();
        }
        audioReorderedPackets = audioSkippedPackets = 0;
        lastTelemetryTick = lastTelemetryFrames = 0;
        lastTelemetryAudioBytes = lastTelemetryPlayerEnqueued = lastTelemetryPlayerTrimmed = 0;
        lastTelemetryPlayerBuffered = 0;
        lastSeq = null;
        waitingForIdr = true;
        currentHasIdr = false;
        ResetAssembly();
    }

    public void Stop()
    {
        var x = cts;
        if (x is null) return;

        try
        {
            var u = videoUdp;
            var ep = switchEndpoint;
            if (u is not null && ep is not null)
            {
                var stop = Encoding.ASCII.GetBytes("MTV2|STOP");
                try { u.Send(stop, stop.Length, ep); } catch { }
            }
        }
        finally
        {
            x.Cancel();
            try { videoUdp?.Close(); } catch { }
            try { audioUdp?.Close(); } catch { }
            Status?.Invoke("VÍDEO: DETENIDO");
        }
    }

    private void Cleanup()
    {
        if (Interlocked.Exchange(ref cleanupGuard, 1) != 0)
            return;

        try { videoUdp?.Dispose(); } catch { }
        videoUdp = null;
        try { audioUdp?.Dispose(); } catch { }
        audioUdp = null;
        switchEndpoint = null;

        try { decoder?.Dispose(); } catch { }
        decoder = null;

        lock (audioGate)
        {
            try { audio?.Dispose(); } catch { }
            audio = null;
        }

        ResetAssembly();

        var x = cts;
        cts = null;
        try { x?.Dispose(); } catch { }

        RunningChanged?.Invoke(false);
    }

    public void Dispose() => Stop();
}
