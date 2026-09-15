using System;
using System.Text;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;

namespace MagicTupperPcClient;

internal static class Program
{
    private static void Main(string[] args)
    {
        Console.Title = "MagicTupper PC Client";
        Console.WriteLine("MagicTupper PC Client");
        Console.WriteLine("Cliente USB MagicTupper · protocolo MTUSB1");
        Console.WriteLine("Buscando Nintendo Switch (USB 057E:3000)...");
        using var context = new UsbContext();
        var finder = new UsbDeviceFinder { Vid = 0x057E, Pid = 0x3000 };
        using var device = context.Find(finder);
        if (device is null)
        {
            Console.WriteLine("No se encontró la Switch. Abra MagicTupper y seleccione 'Conectar SD al PC'.");
            Console.WriteLine("Compruebe que el controlador libusbK está instalado.");
            Console.ReadKey(true);
            return;
        }
        device.Open();
        device.ClaimInterface(0);
        Console.WriteLine("Switch detectada. USB abierto correctamente.");
        var reader = device.OpenEndpointReader(ReadEndpointID.Ep01);
        var writer = device.OpenEndpointWriter(WriteEndpointID.Ep01);
        if (!Exchange(writer, reader, Protocol.Frame(Command.Hello, ReadOnlySpan<byte>.Empty), out var hello))
        {
            Console.WriteLine("No hubo respuesta a HELLO. Compila el NRO con el servidor USB MTUSB1.");
            Console.ReadKey(true);
            return;
        }
        Console.WriteLine($"HELLO correcto: {Encoding.UTF8.GetString(hello)}");
        if (Exchange(writer, reader, Protocol.Frame(Command.Lock, ReadOnlySpan<byte>.Empty), out var locked))
            Console.WriteLine($"LOCK: {Encoding.UTF8.GetString(locked)}");
        else Console.WriteLine("LOCK falló: la sesión USB no se ha reservado.");
        Console.Write("Ruta SD a listar (vacío = raíz): ");
        var requestedPath = Console.ReadLine() ?? string.Empty;
        if (Exchange(writer, reader, Protocol.Frame(Command.List, Protocol.SdPath(requestedPath)), out var listing))
        {
            Console.WriteLine($"Contenido de sdmc:/{requestedPath.Trim('/') }:");
            Console.WriteLine(Encoding.UTF8.GetString(listing));
        }
        else Console.WriteLine("No hubo respuesta a LIST(sdmc:/).");
        Console.Write("Archivo SD para STAT/READ (vacío = salir): ");
        var requestedFile = Console.ReadLine() ?? string.Empty;
        if (!string.IsNullOrWhiteSpace(requestedFile) && Exchange(writer, reader, Protocol.Frame(Command.Stat, Protocol.SdPath(requestedFile)), out var stat))
        {
            var statText = Encoding.UTF8.GetString(stat); Console.WriteLine($"STAT: {statText}");
            var statParts = statText.Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
            if (statParts.Length != 2 || statParts[0] != "F" || !ulong.TryParse(statParts[1], out var total)) Console.WriteLine("READ omitido: STAT no describe un archivo regular.");
            else { Directory.CreateDirectory("MagicTupper-Downloads"); var destination = Path.Combine("MagicTupper-Downloads", Path.GetFileName(requestedFile)); var partial = destination + ".part"; using var output = new FileStream(partial, FileMode.OpenOrCreate, FileAccess.Write, FileShare.Read, 1024 * 1024, FileOptions.SequentialScan); ulong offset = (ulong)output.Length; if (offset > total) { output.SetLength(0); offset = 0; } while (offset < total) { var wanted = (uint)Math.Min(512 * 1024UL, total - offset); if (!Exchange(writer, reader, Protocol.Frame(Command.Read, Protocol.ReadRequest(requestedFile, offset, wanted)), out var content) || content.Length == 0) { Console.WriteLine($"READ falló en offset {offset}."); break; } output.Position = (long)offset; output.Write(content, 0, content.Length); output.Flush(); offset += (uint)content.Length; Console.WriteLine($"READ: {offset}/{total} bytes ({100.0 * offset / total:0.0}%)"); } if (offset == total) { output.Close(); File.Move(partial, destination, true); Console.WriteLine($"READ completado: {total} bytes guardados en {destination}"); } else Console.WriteLine($"Transferencia parcial conservada en {partial}"); }
        }
        else if (!string.IsNullOrWhiteSpace(requestedFile)) Console.WriteLine("STAT falló: el archivo no existe o no es accesible.");
        Console.Write("Archivo local para WRITE (vacío = salir): ");
        var localFile = Console.ReadLine() ?? string.Empty;
        if (!string.IsNullOrWhiteSpace(localFile) && File.Exists(localFile))
        {
            Console.Write("Destino SD relativo (ej. switch/archivo.xci): "); var remoteFile = Console.ReadLine() ?? string.Empty;
            var info = new FileInfo(localFile); using var input = new FileStream(localFile, FileMode.Open, FileAccess.Read, FileShare.Read, 4 * 1024 * 1024, FileOptions.SequentialScan); var offset = 0UL; var buffer = new byte[1024 * 1024];
            while (offset < (ulong)info.Length)
            {
                var wanted = input.Read(buffer, 0, (int)Math.Min((ulong)buffer.Length, (ulong)info.Length - offset)); if (wanted == 0) break;
                if (!Exchange(writer, reader, Protocol.Frame(Command.Write, Protocol.WriteRequest(remoteFile, offset, buffer.AsSpan(0, wanted))), out var written) || !Encoding.UTF8.GetString(written).StartsWith("OK ")) { Console.WriteLine($"WRITE falló en offset {offset}."); break; }
                offset += (uint)wanted; Console.WriteLine($"WRITE: {offset}/{info.Length} bytes ({100.0 * offset / info.Length:0.0}%)");
            }
            if (offset == (ulong)info.Length && Exchange(writer, reader, Protocol.Frame(Command.Write, Protocol.WriteRequest(remoteFile, offset, ReadOnlySpan<byte>.Empty)), out var committed) && Encoding.UTF8.GetString(committed) == "COMMITTED") Console.WriteLine("WRITE completado. El archivo se ha creado en la SD.");
        }
        else if (!string.IsNullOrWhiteSpace(localFile)) Console.WriteLine("WRITE: el archivo local no existe.");
        Console.WriteLine();
        Console.WriteLine("Comandos: HELLO, LIST, STAT, READ, WRITE, MKDIR, REMOVE, LOCK y UNMOUNT.");
        Console.WriteLine("Pulse una tecla para salir.");
        Console.ReadKey(true);
    }

    private static bool Exchange(UsbEndpointWriter writer, UsbEndpointReader reader, byte[] frame, out byte[] payload)
    {
        payload = Array.Empty<byte>();
        writer.Write(frame, 3000, out _);
        var buffer = new byte[Protocol.MaxFrame + 12];
        var chunk = new byte[64 * 1024];
        reader.Read(buffer, 5000, out var count);
        if (count < 12) return false;
        if (Encoding.ASCII.GetString(buffer, 0, 6) != Protocol.Magic) return false;
        var length = BitConverter.ToInt32(buffer, 8);
        if (length < 0 || length > Protocol.MaxFrame || count > length + 12) return false;
        while (count < length + 12)
        {
            reader.Read(chunk, 5000, out var received);
            if (received <= 0) return false;
            if (count + received > buffer.Length) return false;
            Array.Copy(chunk, 0, buffer, count, received);
            count += received;
        }
        payload = buffer[12..(12 + length)];
        return true;
    }
}
