using System.Net;
using System.Net.Sockets;
using System.Text;

namespace MagicTupperPcClient;

// Awoo/Tinfoil-inspired pull model: the PC serves byte ranges and the Switch pulls them.
// Raw TcpListener avoids HttpListener URLACL/admin requirements on Windows.
internal sealed class HttpRangeBenchServer : IDisposable
{
    private readonly TcpListener listener;
    private readonly CancellationTokenSource cts = new();
    private readonly Task acceptTask;
    private readonly ulong virtualLength;
    private readonly byte[] zero = new byte[1024 * 1024];
    private long completedRequests;
    private long bytesSent;

    public int Port => ((IPEndPoint)listener.LocalEndpoint).Port;
    public long CompletedRequests => Interlocked.Read(ref completedRequests);
    public long BytesSent => Interlocked.Read(ref bytesSent);

    public HttpRangeBenchServer(ulong length)
    {
        virtualLength = length;
        listener = new TcpListener(IPAddress.Any, 0);
        listener.Start(16);
        acceptTask = Task.Run(AcceptLoop);
    }

    private async Task AcceptLoop()
    {
        while (!cts.IsCancellationRequested)
        {
            try
            {
                var client = await listener.AcceptTcpClientAsync(cts.Token);
                _ = Task.Run(() => Handle(client), cts.Token);
            }
            catch (OperationCanceledException) { break; }
            catch (ObjectDisposedException) { break; }
            catch { if (!cts.IsCancellationRequested) await Task.Delay(10); }
        }
    }

    private async Task Handle(TcpClient client)
    {
        using (client)
        {
            client.NoDelay = true;
            client.SendBufferSize = 1024 * 1024;
            client.ReceiveBufferSize = 256 * 1024;
            using var stream = client.GetStream();
            stream.WriteTimeout = 30000;
            stream.ReadTimeout = 10000;

            var headerBuf = new byte[16 * 1024];
            var used = 0;
            while (used < headerBuf.Length)
            {
                var n = await stream.ReadAsync(headerBuf.AsMemory(used, headerBuf.Length - used), cts.Token);
                if (n <= 0) return;
                used += n;
                var text = Encoding.ASCII.GetString(headerBuf, 0, used);
                if (text.Contains("\r\n\r\n", StringComparison.Ordinal)) break;
            }
            var request = Encoding.ASCII.GetString(headerBuf, 0, used);
            ulong first = 0, last = virtualLength == 0 ? 0 : virtualLength - 1;
            foreach (var line in request.Split("\r\n"))
            {
                if (!line.StartsWith("Range:", StringComparison.OrdinalIgnoreCase)) continue;
                var value = line[(line.IndexOf(':') + 1)..].Trim();
                if (!value.StartsWith("bytes=", StringComparison.OrdinalIgnoreCase)) continue;
                var parts = value[6..].Split('-', 2);
                if (parts.Length > 0 && ulong.TryParse(parts[0], out var a)) first = a;
                if (parts.Length > 1 && parts[1].Length > 0 && ulong.TryParse(parts[1], out var b)) last = b;
            }
            if (virtualLength == 0 || first >= virtualLength) return;
            if (last >= virtualLength) last = virtualLength - 1;
            if (last < first) return;
            var count = last - first + 1;

            var headers = $"HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Range: bytes {first}-{last}/{virtualLength}\r\nContent-Length: {count}\r\nConnection: close\r\n\r\n";
            var hb = Encoding.ASCII.GetBytes(headers);
            await stream.WriteAsync(hb, cts.Token);
            ulong sent = 0;
            while (sent < count && !cts.IsCancellationRequested)
            {
                var n = (int)Math.Min((ulong)zero.Length, count - sent);
                await stream.WriteAsync(zero.AsMemory(0, n), cts.Token);
                sent += (ulong)n;
            }
            if (sent == count)
            {
                Interlocked.Increment(ref completedRequests);
                Interlocked.Add(ref bytesSent, checked((long)sent));
            }
        }
    }

    public void Dispose()
    {
        cts.Cancel();
        try { listener.Stop(); } catch { }
        try { acceptTask.Wait(1000); } catch { }
        cts.Dispose();
    }
}
