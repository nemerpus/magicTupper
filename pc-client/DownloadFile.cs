namespace MagicTupperPcClient;

internal static class DownloadFile
{
    public static void Receive(string destination, long size, Func<ulong, uint, byte[]> read,
        CancellationToken token, Action<long> progress)
    {
        if (size < 0) throw new IOException("Tamaño remoto inválido.");
        var partial = destination + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            using (var output = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                1024 * 1024, FileOptions.SequentialScan))
            {
                long received = 0;
                while (received < size)
                {
                    token.ThrowIfCancellationRequested();
                    var wanted = (uint)Math.Min(512 * 1024, size - received);
                    var data = read((ulong)received, wanted);
                    if (data.Length != wanted) throw new IOException($"Lectura incompleta en {received}: {data.Length}/{wanted} bytes.");
                    output.Write(data);
                    received += data.Length;
                    progress(received);
                }
                token.ThrowIfCancellationRequested();
                output.Flush(true);
            }
            File.Move(partial, destination, true);
        }
        finally
        {
            if (File.Exists(partial)) File.Delete(partial);
        }
    }
}
