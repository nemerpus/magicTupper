namespace MagicTupperPcClient.Services;

internal enum TransferState { Pending, Active, Paused, Completed, Failed, Cancelled }

internal sealed class TransferItem
{
    public required string Source { get; init; }
    public required string Destination { get; init; }
    public long TotalBytes { get; init; }
    public long CompletedBytes { get; set; }
    public TransferState State { get; set; } = TransferState.Pending;
    public string? Error { get; set; }
    public double Progress => TotalBytes <= 0 ? 0 : Math.Min(1, (double)CompletedBytes / TotalBytes);
}

internal sealed class TransferQueue
{
    private readonly object gate = new();
    private readonly List<TransferItem> items = new();
    public IReadOnlyList<TransferItem> Items { get { lock (gate) return items.ToArray(); } }
    public TransferItem Enqueue(string source, string destination, long totalBytes) { var item = new TransferItem { Source=source, Destination=destination, TotalBytes=totalBytes }; lock(gate) items.Add(item); return item; }
    public void Cancel(TransferItem item) { lock(gate) if(item.State is TransferState.Pending or TransferState.Active or TransferState.Paused) item.State=TransferState.Cancelled; }
    public void Pause(TransferItem item) { lock(gate) if(item.State==TransferState.Active)item.State=TransferState.Paused; }
    public void Resume(TransferItem item) { lock(gate) if(item.State==TransferState.Paused)item.State=TransferState.Active; }
}
