using NAudio.Wave;

namespace MagicTupperPcClient;

/// <summary>
/// Low-latency player for the Switch grc:d audio stream.
/// PCM signed 16-bit little-endian, stereo, 48 kHz, delivered as RTP PT 97.
/// Keeps a small jitter cushion but actively trims stale audio so latency cannot grow.
/// </summary>
internal sealed class NetworkPcmAudioPlayer : IDisposable
{
    private const int SampleRate = 48000;
    private const int BytesPerFrame = 4; // S16 stereo
    private const int BytesPerSecond = SampleRate * BytesPerFrame;
    private const int PrebufferMs = 85;
    private const int TargetBufferMs = 110;
    private const int MaxBufferMs = 220;
    private const int RebufferThresholdMs = 25;

    private readonly object gate = new();
    private WaveOutEvent? output;
    private BufferedWaveProvider? buffer;
    private long lastDataTick;
    private long lastResyncStatusTick;
    private long resyncCount;
    private long rebufferCount;
    private long enqueuedBytes;
    private long trimmedBytes;
    private bool started;
    private bool playbackStarted;
    private byte[] discardScratch = new byte[BytesPerSecond / 10];

    public event Action<string>? Status;

    public int DeviceNumber { get; set; } = -1;
    private float volume = 0.8f;
    public float Volume
    {
        get { lock (gate) return volume; }
        set { SetVolume(value); }
    }

    public void SetVolume(float value)
    {
        lock (gate)
        {
            volume = Math.Clamp(value, 0f, 1f);
            // Do not use WaveOutEvent.Volume here. NAudio maps it to waveOutSetVolume,
            // which can fail with BadDriverId on some Windows audio endpoints.
            // Keep the requested value for UI/state; playback uses the Windows endpoint volume.
        }
    }

    public long EnqueuedBytes { get { lock (gate) return enqueuedBytes; } }
    public long TrimmedBytes { get { lock (gate) return trimmedBytes; } }
    public long TrimCount { get { lock (gate) return resyncCount; } }
    public long RebufferCount { get { lock (gate) return rebufferCount; } }

    public int BufferedBytes
    {
        get { lock (gate) return buffer?.BufferedBytes ?? 0; }
    }

    public int BufferedMilliseconds
    {
        get
        {
            lock (gate)
                return buffer is null ? 0 : (int)Math.Round(buffer.BufferedDuration.TotalMilliseconds);
        }
    }

    public void Start()
    {
        lock (gate)
        {
            if (started) return;
            var provider = new BufferedWaveProvider(new WaveFormat(SampleRate, 16, 2))
            {
                // Capacity only. Playback starts after an 85 ms cushion so one 21.3 ms grc:d block
                // cannot immediately starve WaveOut on normal scheduler jitter.
                BufferDuration = TimeSpan.FromMilliseconds(300),
                DiscardOnBufferOverflow = true,
                ReadFully = true
            };
            var wave = new WaveOutEvent
            {
                DeviceNumber = DeviceNumber,
                DesiredLatency = 50,
                NumberOfBuffers = 3
            };
            wave.Init(provider);
            buffer = provider;
            output = wave;
            started = true;
            playbackStarted = false;
            lastDataTick = 0;
            Status?.Invoke("AUDIO: preparado · PCM 48 kHz estéreo · baja latencia");
        }
    }

    public void Enqueue(ReadOnlySpan<byte> pcm)
    {
        if (pcm.Length < BytesPerFrame) return;
        var usable = pcm.Length & ~(BytesPerFrame - 1);
        if (usable <= 0) return;

        lock (gate)
        {
            if (!started || buffer is null || output is null) return;

            // grc:d delivers ~21.3 ms PCM blocks. Keep enough cushion to absorb block/scheduler burstiness.
            // Only trim on a genuinely stale queue; the previous 60 ms ceiling caused repeated PCM cuts.
            var maxBytes = MsToBytes(MaxBufferMs);
            var targetBytes = MsToBytes(TargetBufferMs);
            var projected = buffer.BufferedBytes + usable;
            if (projected > maxBytes)
            {
                var drop = Math.Min(buffer.BufferedBytes, projected - targetBytes);
                drop &= ~(BytesPerFrame - 1);
                while (drop > 0)
                {
                    var n = Math.Min(drop, discardScratch.Length) & ~(BytesPerFrame - 1);
                    if (n <= 0) break;
                    var read = buffer.Read(discardScratch, 0, n);
                    if (read <= 0) break;
                    drop -= read;
                    trimmedBytes += read;
                }
                resyncCount++;
                var statusNow = Environment.TickCount64;
                if (statusNow - lastResyncStatusTick >= 1000)
                {
                    lastResyncStatusTick = statusNow;
                    Status?.Invoke($"AUDIO: resincronizando · buffer {buffer.BufferedDuration.TotalMilliseconds:F0} ms · trims {resyncCount}");
                }
            }

            var bytes = pcm[..usable].ToArray();
            try
            {
                buffer.AddSamples(bytes, 0, bytes.Length);
                enqueuedBytes += bytes.Length;

                // If the queue almost drained, stop feeding silence and rebuffer instead of
                // oscillating between underrun and aggressive trims (the "gear" sound).
                if (playbackStarted && buffer.BufferedDuration.TotalMilliseconds <= RebufferThresholdMs)
                {
                    try { output.Pause(); } catch { }
                    playbackStarted = false;
                    rebufferCount++;
                }

                // Start/restart with roughly four grc:d audio blocks queued.
                if (!playbackStarted && buffer.BufferedDuration.TotalMilliseconds >= PrebufferMs)
                {
                    output.Play();
                    playbackStarted = true;
                }

                var now = Environment.TickCount64;
                if (lastDataTick == 0 || now - lastDataTick > 3000)
                    Status?.Invoke($"AUDIO: ACTIVO · PCM 48 kHz estéreo · buffer {buffer.BufferedDuration.TotalMilliseconds:F0} ms");
                lastDataTick = now;
            }
            catch (InvalidOperationException)
            {
                // Never retain old audio after an output-device stall.
                buffer.ClearBuffer();
                playbackStarted = false;
                try { output.Stop(); } catch { }
            }
        }
    }

    private static int MsToBytes(int ms)
        => ((BytesPerSecond * ms / 1000) & ~(BytesPerFrame - 1));

    public void FlushAndPause()
    {
        lock (gate)
        {
            try { output?.Pause(); } catch { }
            try { buffer?.ClearBuffer(); } catch { }
            playbackStarted = false;
            lastDataTick = 0;
        }
    }

    public void Resume()
    {
        lock (gate)
        {
            try { buffer?.ClearBuffer(); } catch { }
            playbackStarted = false;
            lastDataTick = 0;
            // Enqueue() will restart WaveOut after a fresh, tiny prebuffer.
        }
    }

    public void Dispose()
    {
        lock (gate)
        {
            started = false;
            playbackStarted = false;
            try { output?.Stop(); } catch { }
            try { output?.Dispose(); } catch { }
            output = null;
            buffer = null;
        }
    }
}
