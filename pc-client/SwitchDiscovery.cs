using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;

namespace MagicTupperPcClient;

internal readonly record struct SwitchDiscoveryResult(string Address, string Banner);

internal static class SwitchDiscovery
{
    private static readonly byte[] Probe = Encoding.ASCII.GetBytes("MTDISC1");

    public static async Task<SwitchDiscoveryResult?> FindAsync(int timeoutMs = 900, CancellationToken token = default)
    {
        using var udp = new UdpClient(AddressFamily.InterNetwork) { EnableBroadcast = true };
        udp.Client.Bind(new IPEndPoint(IPAddress.Any, 0));

        var targets = new HashSet<IPAddress> { IPAddress.Broadcast };
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var u in nic.GetIPProperties().UnicastAddresses)
            {
                if (u.Address.AddressFamily != AddressFamily.InterNetwork || u.IPv4Mask is null) continue;
                var a = u.Address.GetAddressBytes(); var m = u.IPv4Mask.GetAddressBytes(); var b = new byte[4];
                for (var i = 0; i < 4; i++) b[i] = (byte)(a[i] | ~m[i]);
                targets.Add(new IPAddress(b));
            }
        }

        foreach (var target in targets)
        {
            try { await udp.SendAsync(Probe, new IPEndPoint(target, RemotePlayProtocol.InputPort), token); } catch { }
        }

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(timeoutMs);
        try
        {
            while (!timeout.IsCancellationRequested)
            {
                var r = await udp.ReceiveAsync(timeout.Token);
                var text = Encoding.ASCII.GetString(r.Buffer);
                if (text.StartsWith("MTHERE1|", StringComparison.Ordinal))
                    return new SwitchDiscoveryResult(r.RemoteEndPoint.Address.ToString(), text);
            }
        }
        catch (OperationCanceledException) { }
        catch (SocketException) { }
        return null;
    }
}
