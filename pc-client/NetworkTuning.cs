namespace MagicTupperPcClient;

internal readonly record struct NetworkProfile(int ReceiveBuffer, int ChunkSize, bool NoDelay)
{
    public static NetworkProfile Default => new(1024 * 1024, 256 * 1024, false);
    public override string ToString() => $"RCV {ReceiveBuffer / 1024} KiB / bloque {ChunkSize / 1024} KiB / NODELAY {(NoDelay ? "ON" : "OFF")}";
}

internal static class NetworkTuning
{
    // Compare complete profiles in alternating order so drift cannot reward the last option.
    public static (NetworkProfile Profile, double Rate) Select(
        Func<NetworkProfile, string, double> measure, Action<string> log, CancellationToken token)
    {
        var baseline = NetworkProfile.Default;
        var candidates = new[] { baseline, baseline with { ReceiveBuffer = 2 * 1024 * 1024 },
            baseline with { ChunkSize = 64 * 1024 }, baseline with { ChunkSize = 1024 * 1024 },
            baseline with { NoDelay = true } };
        var samples = candidates.ToDictionary(p => p, _ => new List<double>());
        for (var round = 0; round < 3; round++)
        {
            foreach (var profile in round % 2 == 0 ? candidates : candidates.Reverse())
            {
                token.ThrowIfCancellationRequested();
                var rate = measure(profile, $"muestra {round + 1}/3");
                if (!double.IsFinite(rate) || rate <= 0) throw new IOException("Benchmark incompleto; perfil no validado.");
                samples[profile].Add(rate);
            }
        }
        double Median(NetworkProfile p) => samples[p].Order().ElementAt(1);
        var best = baseline;
        foreach (var p in candidates)
        {
            log($"RED mediana: {Median(p):0.00} MiB/s / {p}");
            if (Median(p) > Median(best) * 1.05) best = p;
        }
        token.ThrowIfCancellationRequested();
        var confirmed = measure(best, "confirmación sostenida");
        if (!double.IsFinite(confirmed) || confirmed <= 0) throw new IOException("Confirmación de red fallida.");
        if (confirmed < Median(best) * 0.85)
        {
            log("RED variable: la confirmación cae más del 15%. Restaurando y midiendo el perfil base.");
            best = baseline;
            confirmed = measure(best, "restauración del perfil base");
            if (!double.IsFinite(confirmed) || confirmed <= 0) throw new IOException("No se pudo restaurar el perfil base.");
        }
        return (best, confirmed);
    }
}
