using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace MagicTupperPcClient;

internal static class UsbDebug
{
    public static bool Enabled { get; } = Environment.GetEnvironmentVariable("MAGICTUPPER_DEBUG") == "1";
    private static readonly object _logLock = new();
    private static int _seq = 0;
    private static string? _logPath;
    private static readonly string _logDir;

    static UsbDebug()
    {
        _logDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs");
        Directory.CreateDirectory(_logDir);
        _logPath = Path.Combine(_logDir, "usb-debug.log");
    }

    private static string Ts()
    {
        var now = DateTime.Now;
        var tid = Environment.CurrentManagedThreadId;
        var seq = Interlocked.Increment(ref _seq);
        return $"[{seq:D6}][{now:HH:mm:ss.fff}][T{tid:D2}]";
    }

    public static void Log(string category, string fmt, params object[] args)
    {
        if (!Enabled && category is not ("CONNECT" or "TRANSPORT" or "TRANSFER") && !fmt.Contains("FAIL") && !fmt.Contains("ERROR")) return;
        var msg = $"{Ts()}[PC][{category}] {string.Format(fmt, args)}";
        lock (_logLock)
        {
            try
            {
                File.AppendAllText(_logPath!, msg + Environment.NewLine);
            }
            catch { }
        }
        System.Diagnostics.Debug.WriteLine(msg);
    }

    public static void LogHex(string category, byte[] data, int offset = 0, int? len = null, int maxBytes = 256)
    {
        if (!Enabled) return;
        var l = Math.Min(len ?? data.Length - offset, maxBytes);
        var hex = BitConverter.ToString(data, offset, l).Replace("-", " ");
        if (l < (len ?? data.Length - offset)) hex += " ...";
        Log(category, "[HEX] len={0}: {1}", l, hex);
    }

    public static void LogGate(string action, byte cmd)
    {
        Log("GATE", "{0} cmd=0x{1:X2}", action, cmd);
    }

    public static bool IsMtp2Data(byte[] frame)
        => frame.Length >= 32 && frame[0] == (byte)'M' && frame[1] == (byte)'T' && frame[2] == (byte)'P' && frame[3] == (byte)'2' && frame[4] == 2 && frame[5] is 0x11 or 0x21 or 0x27;
}

internal interface ITransport : IDisposable
{
    bool Write(byte[] frame, int timeoutMs, out string error);
    bool Read(byte[] buffer, int offset, int timeoutMs, out int count, out string error);
    bool IsConnected { get; }
}

internal enum TransportKind { Usb, Tcp }

// Transporte USB sobre LibUsbDotNet (bulk EP1).
internal sealed class UsbTransport : ITransport
{
    private readonly object gate = new();
    private readonly UsbContext context;
    private readonly IUsbDevice device;
    private readonly UsbEndpointReader reader;
    private readonly UsbEndpointWriter writer;

    public UsbTransport()
    {
        UsbDebug.Log("TRANSPORT", "[OPEN] Creating UsbTransport");
        context = new UsbContext();
        device = context.Find(new UsbDeviceFinder { Vid = 0x057E, Pid = 0x3000 })
            ?? throw new InvalidOperationException("Switch no detectada por USB (VID 057E, PID 3000).");
        device.Open();
        device.ClaimInterface(0);
        reader = device.OpenEndpointReader(ReadEndpointID.Ep01);
        writer = device.OpenEndpointWriter(WriteEndpointID.Ep01);
        UsbDebug.Log("TRANSPORT", "[OPEN] Device opened, interface 0 claimed, EP_IN=0x81 EP_OUT=0x01");
    }

    public bool IsConnected => device?.IsOpen == true;

    public bool Write(byte[] frame, int timeoutMs, out string error)
    {
        var quietData = UsbDebug.IsMtp2Data(frame);
        if (!quietData) UsbDebug.LogGate("waiting", frame.Length >= 8 ? frame[7] : (byte)0xFF);
        lock (gate)
        {
            if (!quietData)
            {
                UsbDebug.LogGate("acquired", frame.Length >= 8 ? frame[7] : (byte)0xFF);
                UsbDebug.Log("USB", "[TX] cmd=0x{0:X2} len={1} timeout={2}", frame.Length >= 8 ? frame[7] : 0xFF, frame.Length, timeoutMs);
                if (frame.Length >= 8 && (frame[7] == (byte)Command.TransferData || frame[7] == (byte)Command.DirectData || frame[7] == (byte)Command.LinkBenchData)) UsbDebug.Log("USB", "[TX_DATA] cmd=0x{0:X2} frameLen={1}", frame[7], frame.Length); else UsbDebug.LogHex("USB", frame);
            }

            int bytesWritten = 0;
            var status = writer.Write(frame, timeoutMs, out bytesWritten);
            int numericStatus = Convert.ToInt32(status);

            string errorName = status.ToString();
            string details = $"status={status} numeric={numericStatus} bytesWritten={bytesWritten} requested={frame.Length} endpoint=0x01";

            if (numericStatus == 0 && bytesWritten == frame.Length)
            {
                error = "";
                if (!quietData) { UsbDebug.Log("USB", "[TX] {0}", details); UsbDebug.LogGate("released", frame.Length >= 8 ? frame[7] : (byte)0xFF); }
                return true;
            }
            if (numericStatus == 0 && bytesWritten != frame.Length)
            {
                error = $"USB WRITE corto. bytesWritten={bytesWritten} requested={frame.Length}";
                UsbDebug.Log("USB", "[TX] SHORT_WRITE {0}", details);
                if (!quietData) UsbDebug.LogGate("released", frame.Length >= 8 ? frame[7] : (byte)0xFF);
                return false;
            }

            // Mapear códigos de error LibUsbDotNet a nombres legibles
            string friendly = numericStatus switch
            {
                -1 => "ERROR_IO",
                -2 => "ERROR_INVALID_PARAM",
                -3 => "ERROR_ACCESS",
                -4 => "ERROR_NO_DEVICE",
                -5 => "ERROR_NOT_FOUND",
                -6 => "ERROR_BUSY",
                -7 => "ERROR_TIMEOUT",
                -8 => "ERROR_OVERFLOW",
                -9 => "ERROR_PIPE",
                -10 => "ERROR_INTERRUPTED",
                -11 => "ERROR_NO_MEM",
                -12 => "ERROR_NOT_SUPPORTED",
                -99 => "ERROR_OTHER",
                _ => $"UNKNOWN({numericStatus})"
            };

            error = $"USB WRITE falló ({friendly} / {numericStatus}). bytesWritten={bytesWritten} requested={frame.Length}";
            UsbDebug.Log("USB", "[TX] WRITE_FAIL {0} friendly={1}", details, friendly);
            if (!quietData) UsbDebug.LogGate("released", frame.Length >= 8 ? frame[7] : (byte)0xFF);
            return false;
        }
    }

    public bool Read(byte[] buffer, int offset, int timeoutMs, out int count, out string error)
    {
        UsbDebug.LogGate("waiting_read", 0xFF);
        lock (gate)
        {
            UsbDebug.LogGate("acquired_read", 0xFF);
            int requested = buffer.Length - offset;
            UsbDebug.Log("USB", "[RX] requested={0} timeout={1}", requested, timeoutMs);

            var status = reader.Read(buffer, offset, requested, timeoutMs, out count);
            int numericStatus = Convert.ToInt32(status);

            string errorName = status.ToString();
            string details = $"status={status} numeric={numericStatus} received={count} requested={requested} endpoint=0x81";

            if (numericStatus == 0)
            {
                error = "";
                if (count > 0) UsbDebug.LogHex("USB", buffer, offset, count);
                UsbDebug.Log("USB", "[RX] READ_OK {0}", details);
                UsbDebug.LogGate("released_read", 0xFF);
                return true;
            }

            string friendly = numericStatus switch
            {
                -1 => "ERROR_IO",
                -2 => "ERROR_INVALID_PARAM",
                -3 => "ERROR_ACCESS",
                -4 => "ERROR_NO_DEVICE",
                -5 => "ERROR_NOT_FOUND",
                -6 => "ERROR_BUSY",
                -7 => "ERROR_TIMEOUT",
                -8 => "ERROR_OVERFLOW",
                -9 => "ERROR_PIPE",
                -10 => "ERROR_INTERRUPTED",
                -11 => "ERROR_NO_MEM",
                -12 => "ERROR_NOT_SUPPORTED",
                -99 => "ERROR_OTHER",
                _ => $"UNKNOWN({numericStatus})"
            };

            error = $"USB READ falló ({friendly} / {numericStatus}). received={count} requested={requested}";
            UsbDebug.Log("USB", "[RX] READ_FAIL {0} friendly={1}", details, friendly);
            if (count > 0) UsbDebug.LogHex("USB", buffer, offset, count);
            UsbDebug.LogGate("released_read", 0xFF);
            return false;
        }
    }

    public void Dispose()
    {
        UsbDebug.Log("TRANSPORT", "[CLOSE] Disposing UsbTransport");
        try { device?.Close(); } catch { }
        try { device?.Dispose(); } catch { }
        try { context?.Dispose(); } catch { }
    }
}

// Transporte de red: MTUSB1 sobre TCP al puerto 8766 del NRO.
internal sealed class TcpTransport : ITransport
{
    private readonly object gate = new();
    private readonly TcpClient client;
    private readonly NetworkStream stream;
    public NetworkProfile Profile { get; private set; } = NetworkProfile.Default;

    public void ApplyProfile(NetworkProfile profile)
    {
        lock (gate) { client.NoDelay = profile.NoDelay; Profile = profile; }
    }

    public TcpTransport(string host, int port)
    {
        UsbDebug.Log("TRANSPORT", "[TCP_OPEN] Connecting to {0}:{1}", host, port);
        client = new TcpClient
        {
            NoDelay = false,
            SendBufferSize = 4 * 1024 * 1024,
            ReceiveBufferSize = 4 * 1024 * 1024
        };
        client.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
        client.Connect(host, port);
        stream = client.GetStream();
        stream.ReadTimeout = 10000;
        stream.WriteTimeout = 5000;
    }

    public bool IsConnected => client?.Connected == true;
    public string LocalAddress
    {
        get
        {
            var address = (client.Client.LocalEndPoint as IPEndPoint)?.Address;
            if (address is null) return "127.0.0.1";
            if (address.IsIPv4MappedToIPv6) address = address.MapToIPv4();
            return address.ToString();
        }
    }

    // MT2.4: perfil de recepción TCP de Horizon. Mantiene el flujo continuo y solo cambia SO_RCVBUF
    // en la Switch. Así elegimos por medición el buffer que mejor funciona en este enlace concreto.
    public bool StreamBenchmarkProfile(ulong totalBytes, int switchReceiveBuffer, int writeChunk, bool noDelay, out TimeSpan elapsed, out ulong switchBytes, out uint switchMilliseconds, out uint actualReceiveBuffer, out string error)
    {
        lock (gate)
        {
            elapsed = TimeSpan.Zero; switchBytes = 0; switchMilliseconds = 0; actualReceiveBuffer = 0;
            try
            {
                client.NoDelay = noDelay;
                writeChunk = Math.Clamp(writeChunk, 16 * 1024, 1024 * 1024);
                var header = new byte[20];
                Encoding.ASCII.GetBytes("MTS4").CopyTo(header, 0);
                BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4, 4), 1);
                BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(8, 4), (uint)switchReceiveBuffer);
                BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(12, 8), totalBytes);
                stream.WriteTimeout = 30000; stream.ReadTimeout = 30000;
                stream.Write(header, 0, header.Length);
                var buffer = new byte[writeChunk];
                ulong sent = 0; var sw = Stopwatch.StartNew();
                while (sent < totalBytes)
                {
                    int n = (int)Math.Min((ulong)buffer.Length, totalBytes - sent);
                    stream.Write(buffer, 0, n);
                    sent += (ulong)n;
                }
                var ack = new byte[20]; int got = 0;
                while (got < ack.Length) { int n = stream.Read(ack, got, ack.Length - got); if (n <= 0) throw new IOException("TCP cerrado antes del ACK MTS4"); got += n; }
                sw.Stop(); elapsed = sw.Elapsed;
                if (!ack.AsSpan(0,4).SequenceEqual(Encoding.ASCII.GetBytes("M4OK"))) throw new IOException("ACK MTS4 inválido");
                switchBytes = BinaryPrimitives.ReadUInt64LittleEndian(ack.AsSpan(4,8));
                switchMilliseconds = BinaryPrimitives.ReadUInt32LittleEndian(ack.AsSpan(12,4));
                actualReceiveBuffer = BinaryPrimitives.ReadUInt32LittleEndian(ack.AsSpan(16,4));
                if (switchBytes != totalBytes) throw new IOException($"MTS4 incompleto: Switch recibió {switchBytes} de {totalBytes}");
                error = ""; return true;
            }
            catch (Exception ex) { Dispose(); error = $"TCP PROFILE falló: {ex.Message}"; return false; }
        }
    }

    // MT2.3 TCP native stream benchmark: one small header, then a continuous byte stream.
    // This deliberately removes MT2 per-frame parsing/ACKs to measure the real TCP/Wi-Fi ceiling.
    public bool StreamBenchmark(ulong totalBytes, out TimeSpan elapsed, out ulong switchBytes, out uint switchMilliseconds, out string error)
    {
        lock (gate)
        {
            elapsed = TimeSpan.Zero; switchBytes = 0; switchMilliseconds = 0;
            try
            {
                var header = new byte[16];
                Encoding.ASCII.GetBytes("MTS3").CopyTo(header, 0);
                BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4, 4), 1);
                BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(8, 8), totalBytes);
                stream.WriteTimeout = 30000; stream.ReadTimeout = 30000;
                stream.Write(header, 0, header.Length);
                var buffer = new byte[1024 * 1024];
                ulong sent = 0; var sw = Stopwatch.StartNew();
                while (sent < totalBytes)
                {
                    int n = (int)Math.Min((ulong)buffer.Length, totalBytes - sent);
                    stream.Write(buffer, 0, n);
                    sent += (ulong)n;
                }
                var ack = new byte[16]; int got = 0;
                while (got < ack.Length) { int n = stream.Read(ack, got, ack.Length - got); if (n <= 0) throw new IOException("TCP cerrado antes del ACK MTS3"); got += n; }
                sw.Stop(); elapsed = sw.Elapsed;
                if (!ack.AsSpan(0,4).SequenceEqual(Encoding.ASCII.GetBytes("M3OK"))) throw new IOException("ACK MTS3 inválido");
                switchBytes = BinaryPrimitives.ReadUInt64LittleEndian(ack.AsSpan(4,8));
                switchMilliseconds = BinaryPrimitives.ReadUInt32LittleEndian(ack.AsSpan(12,4));
                if (switchBytes != totalBytes) throw new IOException($"MTS3 incompleto: Switch recibió {switchBytes} de {totalBytes}");
                error = ""; return true;
            }
            catch (Exception ex) { error = $"TCP STREAM falló: {ex.Message}"; return false; }
        }
    }

    public bool Write(byte[] frame, int timeoutMs, out string error)
    {
        lock (gate)
        {
            try
            {
                var quietData = UsbDebug.IsMtp2Data(frame);
                if (!quietData)
                {
                    UsbDebug.Log("USB", "[TCP_TX] cmd=0x{0:X2} len={1} timeout={2}", frame.Length >= 8 ? frame[7] : 0xFF, frame.Length, timeoutMs);
                    if (frame.Length >= 8 && (frame[7] == (byte)Command.TransferData || frame[7] == (byte)Command.DirectData || frame[7] == (byte)Command.LinkBenchData)) UsbDebug.Log("USB", "[TCP_TX_DATA] cmd=0x{0:X2} frameLen={1}", frame[7], frame.Length); else UsbDebug.LogHex("USB", frame);
                }
                stream.WriteTimeout = timeoutMs;
                stream.Write(frame, 0, frame.Length);
                error = "";
                if (!quietData) UsbDebug.Log("USB", "[TCP_TX] WRITE_OK len={0}", frame.Length);
                return true;
            }
            catch (Exception ex)
            {
                error = $"TCP WRITE falló: {ex.Message}";
                UsbDebug.Log("USB", "[TCP_TX] WRITE_FAIL error={0}", ex.Message);
                return false;
            }
        }
    }

    public bool Read(byte[] buffer, int offset, int timeoutMs, out int count, out string error)
    {
        lock (gate)
        {
            try
            {
                int requested = buffer.Length - offset;
                UsbDebug.Log("USB", "[TCP_RX] requested={0} timeout={1}", requested, timeoutMs);
                stream.ReadTimeout = timeoutMs;
                count = stream.Read(buffer, offset, requested);
                error = count == 0 ? "TCP cerrado por el NRO." : "";
                if (count > 0) UsbDebug.LogHex("USB", buffer, offset, count);
                UsbDebug.Log("USB", "[TCP_RX] READ_OK received={0} requested={1}", count, requested);
                return count > 0;
            }
            catch (Exception ex)
            {
                count = 0;
                error = $"TCP READ falló: {ex.Message}";
                UsbDebug.Log("USB", "[TCP_RX] READ_FAIL error={0}", ex.Message);
                return false;
            }
        }
    }

    public void Dispose()
    {
        UsbDebug.Log("TRANSPORT", "[TCP_CLOSE] Disposing TcpTransport");
        try { stream?.Dispose(); } catch { }
        try { client?.Close(); } catch { }
    }
}
