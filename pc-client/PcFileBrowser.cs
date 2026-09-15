namespace MagicTupperPcClient;

internal static class PcFileBrowser
{
    internal static string Resolve(string path)
    {
        if (path.Length > 4096 || !path.StartsWith("pc:/", StringComparison.Ordinal)) throw new IOException("Ruta PC no valida.");
        var relative = path[4..];
        if (relative.Length > 0 && relative.Split('/').Any(p => p.Length == 0 || p is "." or ".." || p.EndsWith(' ') || p.EndsWith('.') ||
            p.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || p.Any(char.IsControl) ||
            System.Text.RegularExpressions.Regex.IsMatch(p, @"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)", System.Text.RegularExpressions.RegexOptions.IgnoreCase)))
            throw new IOException("Ruta PC no permitida.");
        var full = Path.GetFullPath(Path.Combine(@"C:\", relative.Replace('/', '\\')));
        if (!full.StartsWith(@"C:\", StringComparison.OrdinalIgnoreCase)) throw new IOException("Fuera de C:.");
        var current = @"C:\";
        foreach (var part in relative.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, part);
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) throw new IOException("Enlaces y junctions no disponibles.");
        }
        return full;
    }

    internal static object Listing(string path, int offset)
    {
        if (offset < 0 || offset > 1000000 || offset % 128 != 0) throw new IOException("Pagina no valida.");
        var entries = new DirectoryInfo(Resolve(path)).EnumerateFileSystemInfos()
            .Where(e => (e.Attributes & FileAttributes.ReparsePoint) == 0)
            .OrderByDescending(e => e is DirectoryInfo).ThenBy(e => e.Name, StringComparer.OrdinalIgnoreCase)
            .Skip(offset).Take(129).ToArray();
        return new { entries = entries.Take(128).Select(e => new { name = e.Name,
            path = path.TrimEnd('/') + "/" + e.Name, directory = e is DirectoryInfo,
            size = e is FileInfo f ? f.Length : 0L }).ToArray(), offset, next = entries.Length > 128 ? offset + 128 : -1 };
    }
}
