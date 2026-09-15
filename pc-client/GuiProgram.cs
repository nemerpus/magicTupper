using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Windows.Forms;

namespace MagicTupperPcClient;

internal static class GuiProgram
{
    [STAThread] private static void Main() { ApplicationConfiguration.Initialize(); Application.Run(new MainForm()); }
}

internal sealed class MainForm : Form
{
    private readonly TextBox log = new() { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill, BackColor = Color.FromArgb(19,21,23), ForeColor = Color.FromArgb(207,255,120), Font = new Font("Consolas", 10), BorderStyle = BorderStyle.None, Padding = new Padding(10) };
    private readonly Label connectionStatus = new() { Text = "ARCHIVOS: SIN CONEXIÓN · REMOTE PLAY: DETENIDO", AutoSize = true, ForeColor = Color.FromArgb(207,255,120), BackColor = Color.FromArgb(37,40,42), Padding = new Padding(12,8,12,8), Margin = new Padding(18,4,4,4), Font = new Font("Segoe UI", 9, FontStyle.Bold) };
    private readonly Label transferProgress = new() { Text = "", Dock = DockStyle.Bottom, Height = 34, ForeColor = Color.FromArgb(207,255,120), BackColor = Color.FromArgb(13,31,40), Padding = new Padding(10,8,10,6), Font = new Font("Consolas", 9, FontStyle.Bold), AutoEllipsis = true, Visible = false };
    private readonly TextBox path = new() { Text = "switch", Width = 260 };
    private readonly TextBox consoleIp = new() { Text = "192.168.1.100", Width = 150 };
    private ITransport? transport; private TransportKind? transportKind; private int connectInProgress;
    private int connectionAttempt = 0;
    private int connectionGeneration = 0;
    private int reconnectEnabled = 1;
    private readonly object transportGate = new();
    private readonly byte[] mtp2ReadBuffer = new byte[Mtp2.MaxFrame];
    private readonly byte[] controlReadBuffer = new byte[Protocol.MaxFrame + Protocol.HeaderSize];
    private int lastAdaptiveWindow;
    private bool asynchronousDecision;
    private bool pcExplorerSupported;
    private readonly System.Windows.Forms.Timer pcExplorerTimer = new() { Interval = 300 };
    private int pcExplorerPolling;
    private int pcSharingEnabled;
    private readonly CheckBox sharePc = new() { Text = "Compartir C: (solo lectura)", AutoSize = true, ForeColor = Color.White, Padding = new Padding(8, 8, 0, 0) };
    private readonly TreeView pcTree = new() { Dock = DockStyle.Fill, BackColor = Color.FromArgb(28,30,32), ForeColor = Color.White, BorderStyle = BorderStyle.None, AllowDrop = true };
    private readonly TreeView sdTree = new() { Dock = DockStyle.Fill, BackColor = Color.FromArgb(28,30,32), ForeColor = Color.White, BorderStyle = BorderStyle.None, AllowDrop = true };
    private readonly CheckedListBox queueView = new() { Dock = DockStyle.Fill, BackColor = Color.FromArgb(19,21,23), ForeColor = Color.FromArgb(181,255,57), BorderStyle = BorderStyle.None, HorizontalScrollbar = true, CheckOnClick = true, DrawMode = DrawMode.OwnerDrawFixed, ItemHeight = 30 };
    private string pcPath = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + "\\Downloads";
    private Point dragStart; private string? dragFile;
    private Button? sendTransferButton, cancelTransferButton;
    private CancellationTokenSource? transferCts;
    private int transferRunning;
    private const int TcpPort = 8766;
    private readonly ToolTip tooltips = new();
    private string transportStatusText = "ARCHIVOS: SIN CONEXIÓN";
    private Color transportStatusColor = Color.Silver;
    private string remotePlayStatusText = "REMOTE PLAY: DETENIDO";
    private Color remotePlayStatusColor = Color.Silver;
    private string? lastReconnectMessage;
    private int discoveryInProgress;
    private readonly RemotePlayPanel remotePlay;


    public MainForm(bool preview = false)
    {
        pcExplorerTimer.Tick += (_, _) => PollPcExplorer();
        if (!preview) pcExplorerTimer.Start();
        Text = $"MagicTupper · Portal C-137 · {Application.ProductVersion}"; Width = 1280; Height = 850; MinimumSize = new Size(1000,720); StartPosition = FormStartPosition.CenterScreen; FormBorderStyle = FormBorderStyle.Sizable; AutoScaleMode = AutoScaleMode.Dpi; BackColor = Color.FromArgb(23,25,27); ForeColor = Color.White;
        remotePlay = new RemotePlayPanel(() => consoleIp.Text.Trim(), UpdateRemotePlayStatus);
        var bar = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(12, 4, 12, 4), BackColor = Color.FromArgb(31,34,36), WrapContents = true };
        var brand = new Panel { Dock = DockStyle.Top, Height = 90, Padding = new Padding(18, 8, 18, 8), BackColor = Color.FromArgb(19,21,23) };
        var portrait = new PictureBox { Dock = DockStyle.Right, Width = 120, SizeMode = PictureBoxSizeMode.Zoom };
        using (var asset = typeof(MainForm).Assembly.GetManifestResourceStream("MagicTupper.Rick.jpg"))
            if (asset is not null) { using var bitmap = Image.FromStream(asset); portrait.Image = new Bitmap(bitmap); }
        brand.Controls.Add(portrait);
        brand.Controls.Add(new Label { Text = "MAGICTUPPER", AutoSize = true, Location = new Point(18, 12), ForeColor = Color.FromArgb(181,255,57), Font = new Font("Segoe UI", 23, FontStyle.Bold) });
        brand.Controls.Add(new Label { Text = "C-137 / Morty, los archivos primero. La crisis existencial después.", AutoSize = true, Location = new Point(21, 58), ForeColor = Color.FromArgb(185,189,193), Font = new Font("Segoe UI", 10) });
        var statusBar = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 37, BackColor = Color.FromArgb(19,21,23) };
        connectionStatus.Margin = new Padding(8, 2, 8, 2);
        connectionStatus.Padding = new Padding(8, 6, 8, 6);
        statusBar.Controls.Add(connectionStatus);
        bar.Controls.Add(ToolButton("\uE710", "Añadir archivos", (_, _) => SelectFiles()));
        bar.Controls.Add(new Label { Text = "IP consola", AutoSize = true, ForeColor = Color.White, Padding = new Padding(8,8,0,0) });
        bar.Controls.Add(consoleIp);
        sharePc.CheckedChanged += (_, _) => Volatile.Write(ref pcSharingEnabled, sharePc.Checked ? 1 : 0);
        tooltips.SetToolTip(sharePc, "Autorizar a la consola conectada a leer archivos de C:. Solo en una red de confianza.");
        bar.Controls.Add(sharePc);
        bar.Controls.Add(Button("Conectar USB", (_, _) => RequestConnect(TransportKind.Usb)));
        bar.Controls.Add(Button("Conectar RED", (_, _) => ConnectTcp()));
        bar.Controls.Add(ActionButton("Desconectar", (_, _) => DisconnectManual(), Color.FromArgb(90,35,35), Color.White));
        bar.Controls.Add(ToolButton("\uE72C", "Actualizar SD", (_, _) => Run(List)));
        bar.Controls.Add(ToolButton("\uE9D9", "Benchmark del enlace", (_, _) => RunLinkBenchmark()));
        bar.Controls.Add(ToolButton("\uE716", "Usuarios del portal", (_, _) => { using var accounts = new AccountsForm(); accounts.ShowDialog(this); }));
        bar.Controls.Add(ToolButton("\uE896", "Descargar archivo seleccionado a PC", (_, _) => { if (sdTree.SelectedNode?.Tag is string p && sdTree.SelectedNode.Name == "F") { path.Text = p; ReadFile(p); } }));

        var panels = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, FixedPanel = FixedPanel.Panel2, BackColor = Color.FromArgb(23,25,27) };
        var explorers = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Vertical, SplitterDistance = 600, IsSplitterFixed = false };
        explorers.Resize += (_, _) => { if (explorers.Width > 0) explorers.SplitterDistance = explorers.Width / 2; };

        queueView.DrawMode = DrawMode.Normal;
        queueView.Font = new Font("Segoe UI", 10);
        queueView.IntegralHeight = false;

        pcTree.MouseDown += (_, e) => { var n = pcTree.GetNodeAt(e.Location); if (e.Button == MouseButtons.Right && n is not null) { pcTree.SelectedNode = n; pcTree.Focus(); } if (e.Button == MouseButtons.Left) { dragStart = e.Location; dragFile = n?.Tag as string; } };
        pcTree.MouseMove += (_, e) => { if (e.Button == MouseButtons.Left && dragFile is string p && File.Exists(p) && Math.Abs(e.X - dragStart.X) + Math.Abs(e.Y - dragStart.Y) > 8) { pcTree.DoDragDrop(new DataObject(DataFormats.FileDrop, new[] { p }), DragDropEffects.Copy); dragFile = null; } };
        explorers.Panel1.Controls.Add(Pane("PC", pcTree));
        explorers.Panel2.Controls.Add(Pane("SD DE NINTENDO SWITCH", sdTree));

        var drop = new TableLayoutPanel { Dock = DockStyle.Fill, AllowDrop = true, BackColor = Color.FromArgb(19,21,23), Padding = new Padding(14,10,14,12), ColumnCount = 2, RowCount = 4 };
        drop.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        drop.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 235));
        drop.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        drop.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        drop.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        drop.RowStyles.Add(new RowStyle(SizeType.Absolute, 96));
        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(10, 0, 0, 0) };
        var portalTitle = new Label { Text = "COLA DE TRANSFERENCIA", Dock = DockStyle.Top, Height = 28, ForeColor = Color.FromArgb(181,255,57), BackColor = Color.FromArgb(19,21,23), Font = new Font("Segoe UI", 9, FontStyle.Bold), Padding = new Padding(4,5,0,0) };
        var clear = ActionButton("Vaciar cola", (_, _) => { if (Volatile.Read(ref transferRunning) != 0) return; sendQueue.Clear(); queueView.Items.Clear(); queueView.Items.Add("Arrastra archivos NSP/XCI desde el árbol PC"); queueView.Invalidate(); }, Color.FromArgb(28,30,32), Color.FromArgb(184,201,204));
        clear.Dock = DockStyle.Bottom; clear.Height = 36;
        sendTransferButton = ActionButton("Enviar a Switch", (_, _) => SendQueue(), Color.FromArgb(181,255,57), Color.FromArgb(19,21,23));
        sendTransferButton.Dock = DockStyle.Bottom; sendTransferButton.Height = 46;
        cancelTransferButton = ActionButton("Cancelar operación", (_, _) => CancelTransfer(), Color.FromArgb(90,35,35), Color.White);
        cancelTransferButton.Dock = DockStyle.Bottom; cancelTransferButton.Height = 38; cancelTransferButton.Enabled = false;
        queueView.Dock = DockStyle.Fill; queueView.Margin = new Padding(0, 0, 8, 4);
        foreach (var action in new[] { sendTransferButton, cancelTransferButton, clear }) { action.Dock = DockStyle.None; action.AutoSize = false; action.Width = 215; action.Height = 40; action.Margin = new Padding(0, 0, 0, 8); actions.Controls.Add(action); }
        drop.Controls.Add(portalTitle, 0, 0); drop.SetColumnSpan(portalTitle, 2);
        drop.Controls.Add(actions, 1, 1);
        queueView.Items.Add("Arrastra archivos NSP/XCI desde el árbol PC");
        queueView.MouseUp += (_, e) => { if (e.Button == MouseButtons.Right && queueView.SelectedIndex >= 0) { var menu = new ContextMenuStrip(); menu.Items.Add("Quitar seleccionado", null, (_, _) => { var i = queueView.SelectedIndex; if (i >= 0 && i < sendQueue.Count) { sendQueue.RemoveAt(i); queueView.Items.RemoveAt(i); queueView.Invalidate(); } }); menu.Show(queueView, e.Location); } };
        drop.Controls.Add(queueView, 0, 1);
        DragEventHandler enter = (_, e) => { if (e.Data?.GetDataPresent(DataFormats.FileDrop) == true || e.Data?.GetDataPresent(typeof(string)) == true) e.Effect = DragDropEffects.Copy; };
        DragEventHandler receive = (_, e) => { if (e.Data?.GetData(DataFormats.FileDrop) is string[] files) foreach (var file in files) AddQueueItem(file); else if (e.Data?.GetData(typeof(string)) is string file) AddQueueItem(file); };
        drop.DragEnter += enter; drop.DragDrop += receive; queueView.DragEnter += enter; queueView.DragDrop += receive;

        transferProgress.Dock = DockStyle.Fill; transferProgress.Visible = true; transferProgress.Text = "PORTAL EN ESPERA";
        log.Dock = DockStyle.Fill;
        drop.Controls.Add(transferProgress, 0, 2); drop.SetColumnSpan(transferProgress, 2);
        drop.Controls.Add(log, 0, 3); drop.SetColumnSpan(log, 2);
        panels.Panel1.Controls.Add(explorers);
        var lower = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Vertical, BackColor = Color.FromArgb(23,25,27) };
        // SplitterDistance cannot be configured safely until WinForms has laid out the control.
        // Setting Panel*MinSize in the object initializer can throw at startup because the
        // SplitContainer still has its small default design-time width at that point.
        lower.Panel1MinSize = 0;
        lower.Panel2MinSize = 0;
        lower.Panel1.Controls.Add(drop);
        lower.Panel2.Controls.Add(remotePlay);
        void LayoutLowerSplit()
        {
            if (lower.Width <= lower.SplitterWidth + 2) return;
            const int desiredRemoteWidth = 520;
            const int desiredPortalMin = 420;
            const int desiredRemoteMin = 360;
            int maxLeft = Math.Max(1, lower.Width - desiredRemoteMin - lower.SplitterWidth);
            int target = Math.Clamp(lower.Width - desiredRemoteWidth, Math.Min(desiredPortalMin, maxLeft), maxLeft);
            if (target > 0 && target < lower.Width - lower.SplitterWidth)
                lower.SplitterDistance = target;
        }
        lower.Resize += (_, _) => LayoutLowerSplit();
        panels.Panel2.Controls.Add(lower);
        Controls.Add(panels); Controls.Add(bar); Controls.Add(brand); Controls.Add(statusBar);
        BuildPcTree();
        sdTree.Nodes.Add(new TreeNode("Conectando automáticamente...") { Tag = "" });
        pcTree.NodeMouseDoubleClick += (_, e) => OpenPc(e.Node);
        sdTree.NodeMouseDoubleClick += (_, e) => OpenSd(e.Node);
        sdTree.BeforeExpand += (_, e) => { if (e.Node is TreeNode node) LoadSdNode(node); };
        sdTree.AfterSelect += (_, e) =>
        {
            if (e.Node?.Tag is not string p) return;
            path.Text = p.TrimStart('/');
            // Cargar la carpeta al seleccionarla evita que parezca vacía si el usuario no pulsa exactamente el glyph de expansión.
            if (e.Node.Name == "D" && e.Node.Nodes.Count == 1 && e.Node.Nodes[0].Tag is null) LoadSdNode(e.Node);
        };
        pcTree.ItemDrag += (_, e) => { if (e.Item is TreeNode n && n.Tag is string p && File.Exists(p)) pcTree.DoDragDrop(new DataObject(DataFormats.FileDrop, new[] { p }), DragDropEffects.Copy); };
        sdTree.ItemDrag += (_, e) =>
        {
            if (e.Item is not TreeNode n || n.Name != "F" || n.Tag is not string remote || string.IsNullOrWhiteSpace(remote)) return;
            var data = new DataObject();
            data.SetData("MagicTupper.SdPath", remote);
            sdTree.DoDragDrop(data, DragDropEffects.Copy);
        };
        pcTree.DragEnter += (_, e) =>
        {
            if (e.Data?.GetDataPresent("MagicTupper.SdPath") == true) e.Effect = DragDropEffects.Copy;
        };
        pcTree.DragOver += (_, e) =>
        {
            if (e.Data?.GetDataPresent("MagicTupper.SdPath") != true) return;
            var client = pcTree.PointToClient(new Point(e.X, e.Y));
            var node = pcTree.GetNodeAt(client);
            if (node is not null) pcTree.SelectedNode = node;
            e.Effect = DragDropEffects.Copy;
        };
        pcTree.DragDrop += (_, e) =>
        {
            if (e.Data?.GetData("MagicTupper.SdPath") is not string remote || string.IsNullOrWhiteSpace(remote)) return;
            var client = pcTree.PointToClient(new Point(e.X, e.Y));
            var directory = ResolvePcDropDirectory(client);
            var destination = Path.Combine(directory, Path.GetFileName(remote));
            ReadFile(remote, destination);
        };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Copiar / añadir a bandeja", null, (_, _) => { if (pcTree.SelectedNode?.Tag is string p && File.Exists(p)) AddQueueItem(p); });
        menu.Items.Add("Descargar a PC", null, (_, _) => { if (sdTree.SelectedNode?.Tag is string p && !string.IsNullOrEmpty(p) && sdTree.SelectedNode.Name == "F") { path.Text = p; ReadFile(p); } });
        menu.Items.Add("Eliminar", null, (_, _) => DeleteSelected());
        menu.Items.Add("Propiedades", null, (_, _) => { var p = (pcTree.Focused ? pcTree.SelectedNode?.Tag : sdTree.SelectedNode?.Tag) as string; if (!string.IsNullOrEmpty(p)) MessageBox.Show(p, "Propiedades"); });
        pcTree.ContextMenuStrip = menu; sdTree.ContextMenuStrip = menu;
        DragEventHandler acceptDrag = (_, e) => { if (e.Data?.GetDataPresent(DataFormats.FileDrop) == true || e.Data?.GetDataPresent(DataFormats.Text) == true) e.Effect = DragDropEffects.Copy; };
        DragEventHandler receiveDrag = (_, e) => { if (e.Data?.GetData(DataFormats.FileDrop) is string[] files) foreach (var file in files) AddQueueItem(file); else if (e.Data?.GetData(DataFormats.Text) is string file) AddQueueItem(file); };
        drop.DragEnter += acceptDrag; drop.DragDrop += receiveDrag; queueView.DragEnter += acceptDrag; queueView.DragDrop += receiveDrag;
        Shown += (_, _) =>
        {
            LayoutLowerSplit();
            panels.Panel2MinSize = Math.Min(430, Math.Max(0, panels.Height - panels.SplitterWidth - 60));
            panels.SplitterDistance = Math.Max(60, panels.Height - 430);
            if (!preview) _ = DiscoverSwitchAsync();
        };
        if (preview) { connectionStatus.Text = "SIN CONEXIÓN"; sdTree.Nodes.Clear(); sdTree.Nodes.Add("sdmc:/"); }
        Log($"MagicTupper {Application.ProductVersion} · MT2.7 / Laboratorio C-137");
        Log("Conectando automáticamente por USB y cargando la raíz de la SD...");
        Log("También puedes cambiar entre USB y RED (puerto 8766) o desconectar el transporte activo.");
    }

    private Control Pane(string title, Control content) { var p = new Panel { Dock = DockStyle.Fill, BackColor = Color.FromArgb(28,30,32) }; p.Controls.Add(content); p.Controls.Add(new Label { Text = title, Dock = DockStyle.Top, Height = 32, Padding = new Padding(10,8,0,0), ForeColor = Color.FromArgb(181,255,57), BackColor = Color.FromArgb(37,40,42) }); return p; }
    private void BuildPcTree() { pcTree.Nodes.Clear(); var root = new TreeNode("Este equipo") { Tag = "__THIS_PC__" }; pcTree.Nodes.Add(root); try { foreach (var drive in DriveInfo.GetDrives().Where(d => d.IsReady).OrderBy(d => d.Name)) { var label = string.IsNullOrWhiteSpace(drive.VolumeLabel) ? "" : $" ({drive.VolumeLabel})"; var node = new TreeNode($"{drive.Name.TrimEnd('\\')}{label}") { Tag = drive.RootDirectory.FullName }; node.Nodes.Add(new TreeNode()); root.Nodes.Add(node); } } catch { } var downloads = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + "\\Downloads"; if (Directory.Exists(downloads)) { var node = new TreeNode("Descargas") { Tag = downloads }; node.Nodes.Add(new TreeNode()); root.Nodes.Add(node); } root.Expand(); pcTree.BeforeExpand -= PcTreeBeforeExpand; pcTree.BeforeExpand += PcTreeBeforeExpand; }
    private void PcTreeBeforeExpand(object? sender, TreeViewCancelEventArgs e) { var node = e.Node; if (node is not null && node.Tag is string p && p != "__THIS_PC__" && node.Nodes.Count == 1 && node.Nodes[0].Tag is null) LoadPc(node); }
    private void NavigatePc(string? directory) { if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) return; pcPath = directory; pcTree.Nodes.Clear(); var node = new TreeNode(directory) { Tag = directory }; pcTree.Nodes.Add(node); LoadPc(node); node.Expand(); }
    private void LoadPc(TreeNode node) { try { node.Nodes.Clear(); foreach (var d in Directory.EnumerateDirectories((string)node.Tag!)) { var child = new TreeNode(Path.GetFileName(d)) { Tag = d }; child.Nodes.Add(new TreeNode()); node.Nodes.Add(child); } foreach (var f in Directory.EnumerateFiles((string)node.Tag!)) node.Nodes.Add(new TreeNode(Path.GetFileName(f)) { Tag = f }); } catch { } }
    private void OpenPc(TreeNode node) { if (node.Tag is not string p || p == "__THIS_PC__") return; if (Directory.Exists(p)) { if (node.Nodes.Count == 1 && node.Nodes[0].Tag is null) LoadPc(node); node.Expand(); } else if (File.Exists(p)) Log("Seleccionado: " + Path.GetFileName(p)); }
    private string SdParent(string value) { var p = value.Replace('\\', '/').Trim('/'); var i = p.LastIndexOf('/'); return i < 0 ? "" : p[..i]; }
    private void LoadSdNode(TreeNode node)
    {
        if (node.Tag is not string remote || transport is null || Volatile.Read(ref transferRunning) != 0) return;
        if (node.Nodes.Count != 1 || node.Nodes[0].Tag is not null) return;
        node.Nodes.Clear();
        node.Nodes.Add(new TreeNode("Cargando...") { Tag = "__loading__" });
        var generation = Volatile.Read(ref connectionGeneration);
        Run(() =>
        {
            Log($"LIST SD: {remote}");
            var ok = Exchange(Protocol.Frame(Command.List, Protocol.SdPath(remote)), out var data);
            if (IsDisposed) return;
            BeginInvoke(() =>
            {
                if (generation != Volatile.Read(ref connectionGeneration) || node.TreeView != sdTree) return;
                node.Nodes.Clear();
                if (!ok) { node.Nodes.Add(new TreeNode("No se pudo listar esta carpeta") { Tag = null }); Log($"LIST falló: {remote}"); return; }
                var listingText = Encoding.UTF8.GetString(data);
                if (listingText.StartsWith("ERR", StringComparison.OrdinalIgnoreCase)) { node.Nodes.Add(new TreeNode(listingText) { Tag = null }); Log($"LIST rechazado {remote}: {listingText}"); return; }
                foreach (var line in listingText.Split('\n', StringSplitOptions.RemoveEmptyEntries))
                {
                    var parts = line.Split(' ', 2);
                    if (parts.Length != 2) continue;
                    var child = new TreeNode(parts[1]) { Tag = remote.Trim('/') + (remote.Trim('/').Length > 0 ? "/" : "") + parts[1], Name = parts[0] };
                    if (parts[0] == "D") child.Nodes.Add(new TreeNode());
                    node.Nodes.Add(child);
                }
            });
        });
    }
    private void OpenSd(TreeNode node)
    {
        if (node.Tag is not string p) return;
        path.Text = p.TrimStart('/');
        if (node.Name == "D" || node.Nodes.Count > 0)
        {
            LoadSdNode(node);
            node.Expand();
        }
        else if (node.Name == "F")
        {
            ReadFile(p);
        }
    }
    private readonly List<string> sendQueue = new();
    private void AddQueueItem(string file) { if (Volatile.Read(ref transferRunning) != 0) return; var ext = Path.GetExtension(file).ToLowerInvariant(); if (ext is ".xci" or ".nsp") { if (!sendQueue.Contains(file)) { sendQueue.Add(file); if (queueView.Items.Count == 1 && queueView.Items[0] is string marker && marker.StartsWith("Arrastra")) queueView.Items.Clear(); queueView.Items.Add(Path.GetFileName(file) + "    [" + file + "]"); queueView.SetItemChecked(queueView.Items.Count - 1, true); queueView.Invalidate(); Log("Bandeja: " + Path.GetFileName(file) + "\n  " + file); } } else Log("Archivo omitido (solo NSP/XCI): " + file); }
    private void SelectFiles() { using var dialog = new OpenFileDialog { Multiselect = true, Filter = "Nintendo Switch (*.nsp;*.xci)|*.nsp;*.xci|Todos los archivos (*.*)|*.*", Title = "Añadir archivos a la bandeja" }; if (dialog.ShowDialog() == DialogResult.OK) foreach (var file in dialog.FileNames) AddQueueItem(file); }
    private void DeleteSelected() { var isPc = pcTree.Focused; var node = isPc ? pcTree.SelectedNode : sdTree.SelectedNode; var value = node?.Tag as string; if (node is null || string.IsNullOrWhiteSpace(value) || node.Nodes.Count > 0) return; if (MessageBox.Show("¿Eliminar este archivo?\n" + value, "Confirmar eliminación", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return; if (isPc) { try { File.Delete(value); node.Remove(); Log("Eliminado: " + value); } catch (Exception ex) { Log("Error al eliminar: " + ex.Message); } } else Run(() => { if (Exchange(Protocol.Frame(Command.Remove, Protocol.SdPath(value)), out _)) { Log("Eliminado de SD: " + value); BeginInvoke(List); } else Log("No se pudo eliminar en la SD."); }); }
    private void SetTransferUi(bool running)
    {
        if (InvokeRequired) { BeginInvoke(() => SetTransferUi(running)); return; }
        queueView.Enabled = !running;
        sdTree.Enabled = !running;
        if (sendTransferButton is not null) sendTransferButton.Enabled = !running;
        if (cancelTransferButton is not null) cancelTransferButton.Enabled = running;
    }
    private void CancelTransfer()
    {
        var cts = transferCts;
        if (cts is null || cts.IsCancellationRequested) return;
        cts.Cancel();
        Log("Cancelación solicitada. Esperando a que la operación en curso se detenga...");
        // The owning operation drains its current ACK before cancellation.

    }

    private static string NormalizeHttpHost(string address)
    {
        if (!System.Net.IPAddress.TryParse(address, out var ip)) return address;
        if (ip.IsIPv4MappedToIPv6) ip = ip.MapToIPv4();
        return ip.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6 ? $"[{ip}]" : ip.ToString();
    }

    private static bool TryParseHttpPullRate(string text, out double rate)
    {
        rate = 0;
        if (!text.EndsWith("OK", StringComparison.Ordinal)) return false;
        var marker = ": ";
        var start = text.IndexOf(marker, StringComparison.Ordinal);
        if (start < 0) return false;
        start += marker.Length;
        var end = text.IndexOf(" MiB/s", start, StringComparison.Ordinal);
        if (end <= start) return false;
        return double.TryParse(text[start..end], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out rate);
    }

    private void RunLinkBenchmark()
    {
        if (transport is null) { Log("Conecta primero la Switch por USB o RED."); return; }
        if (Interlocked.CompareExchange(ref transferRunning, 1, 0) != 0) { Log("Espera a que termine la operación actual."); return; }
        transferCts?.Dispose();
        transferCts = new CancellationTokenSource();
        SetTransferUi(true);
        Log(transportKind == TransportKind.Usb
            ? "Iniciando benchmark USB MT2.7 · 1 GiB · autotuning 8/16/32/64/128 MiB..."
            : "Iniciando benchmark RED MT2.7 · autotuning Horizon/TCP persistente...");
        Run(() =>
        {
            try
            {
            lock (transportGate)
            {
                if (transportKind == TransportKind.Tcp && transport is TcpTransport tcp)
                {
                    var result = NetworkTuning.Select((profile, label) =>
                    {
                        var bytes = label.Contains("sostenida") ? 128UL * 1024 * 1024 : 32UL * 1024 * 1024;
                        if (!tcp.StreamBenchmarkProfile(bytes, profile.ReceiveBuffer, profile.ChunkSize, profile.NoDelay,
                            out var elapsed, out var swBytes, out var swMs, out var actualRcv, out var error))
                            throw new IOException(error);
                        var pcRate = bytes / 1048576.0 / Math.Max(0.001, elapsed.TotalSeconds);
                        var switchRate = swBytes / 1048576.0 / Math.Max(0.001, swMs / 1000.0);
                        var rate = Math.Min(pcRate, switchRate);
                        Log($"RED {label}: {rate:0.00} MiB/s · {profile} · RCV real {actualRcv / 1024} KiB");
                        return rate;
                    }, Log, transferCts!.Token);
                    tcp.ApplyProfile(result.Profile);
                    Log($"MT2.7 RED aplicado al envío: {result.Profile} · {result.Rate:0.00} MiB/s ({result.Rate * 8 * 1.048576:0.0} Mbit/s).");
                    Log("Benchmark de memoria: la velocidad de copia e instalación se mide por separado.");
                    return;
                }

                if (!Mtp2NegotiateLocked(out var chunkSize, out var maxWindow)) { Log("Benchmark: MT2 no disponible; actualiza también el NRO."); return; }
                const ulong total = 1024UL * 1024 * 1024;
                const ulong sampleBytes = 128UL * 1024 * 1024;
                int[] candidatesMiB = { 8, 16, 32, 64, 128 };
                var candidates = candidatesMiB.Select(m => Math.Min(maxWindow, m * 1024 * 1024)).Distinct().Where(w => w >= chunkSize).ToArray();
                Log($"MT2.7 USB negociado: bloque {chunkSize / 1024} KiB · máximo {maxWindow / 1048576.0:0.#} MiB.");
                var openPayload = new byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(openPayload, total);
                const uint streamId = 0xB006;
                if (!Mtp2ExchangeLocked(Mtp2.Frame(Mtp2Type.BenchmarkOpen, Mtp2Flags.AckRequired, streamId, 0, 0, openPayload), out _, 10000, 5000)) { Log("Benchmark: la Switch no aceptó MT2 BENCH_OPEN."); return; }
                var chunk = new byte[chunkSize]; ulong sent = 0; uint seq = 0; int bestWindow = candidates[0]; double bestRate = 0;
                var fullTimer = Stopwatch.StartNew();

                bool SendFixedWindowSegment(ulong bytesToSend, int fixedWindow, out double segmentRate)
                {
                    var timer = Stopwatch.StartNew(); ulong segmentSent = 0;
                    while (segmentSent < bytesToSend)
                    {
                        transferCts!.Token.ThrowIfCancellationRequested();
                        var windowTarget = Math.Min((ulong)fixedWindow, bytesToSend - segmentSent);
                        ulong windowSent = 0;
                        while (windowSent < windowTarget)
                        {
                            var n = (int)Math.Min((ulong)chunkSize, windowTarget - windowSent);
                            var lastInWindow = windowSent + (ulong)n == windowTarget;
                            var lastOverall = sent + segmentSent + windowSent + (ulong)n == total;
                            var flags = (lastInWindow ? Mtp2Flags.AckRequired : Mtp2Flags.None) | (lastOverall ? Mtp2Flags.EndOfStream : Mtp2Flags.None);
                            var frame = Mtp2.Frame(Mtp2Type.BenchmarkData, flags, streamId, seq++, sent + segmentSent + windowSent, chunk.AsSpan(0, n));
                            if (transport is null) { Log("USB benchmark: transporte no disponible."); segmentRate = 0; return false; }
                            if (!transport.Write(frame, 60000, out var writeError)) { Log("USB benchmark: " + writeError); segmentRate = 0; return false; }
                            windowSent += (ulong)n;
                        }
                        var absolute = sent + segmentSent + windowSent;
                        if (!Mtp2ReadLocked(out var ack, out _, 60000) || (byte)ack.Type != ((byte)Mtp2Type.BenchmarkData | 0x80) || ack.StreamId != streamId || ack.Offset != absolute)
                        { Log("USB benchmark: ACK acumulativo inválido."); segmentRate = 0; return false; }
                        segmentSent += windowSent;
                    }
                    timer.Stop(); segmentRate = bytesToSend / 1048576.0 / Math.Max(0.001, timer.Elapsed.TotalSeconds); return true;
                }

                foreach (var candidate in candidates)
                {
                    var amount = Math.Min(sampleBytes, total - sent);
                    if (!SendFixedWindowSegment(amount, candidate, out var rate)) return;
                    sent += amount; Log($"USB perfil: ventana {candidate / 1048576.0:0.#} MiB · {rate:0.00} MiB/s");
                    if (bestRate == 0 || rate > bestRate * 1.01) { bestRate = rate; bestWindow = candidate; }
                }
                Log($"USB ventana elegida: {bestWindow / 1048576.0:0.#} MiB · mejor muestra {bestRate:0.00} MiB/s.");
                if (sent < total)
                {
                    var remain = total - sent;
                    if (!SendFixedWindowSegment(remain, bestWindow, out var sustained)) return;
                    sent += remain; Log($"USB tramo sostenido: {remain / 1048576.0:0} MiB · {sustained:0.00} MiB/s");
                }
                if (!Mtp2ExchangeLocked(Mtp2.Frame(Mtp2Type.BenchmarkClose, Mtp2Flags.AckRequired, streamId, seq, sent, ReadOnlySpan<byte>.Empty), out var end, 10000, 5000)) { Log("Benchmark: no se pudo leer el resultado final MT2."); return; }
                fullTimer.Stop();
                var totalRate = total / 1048576.0 / Math.Max(0.001, fullTimer.Elapsed.TotalSeconds);
                Log($"BENCHMARK USB MT2.7 PC→Switch: {totalRate:0.00} MiB/s · {total / 1048576} MiB · {fullTimer.Elapsed.TotalSeconds:0.00}s · ventana elegida {bestWindow / 1048576.0:0.#} MiB");
                lastAdaptiveWindow = bestWindow;
                if (end.Length > 0) Log("Switch: " + Encoding.UTF8.GetString(end));
            }
            }
            catch (OperationCanceledException) { Log("Benchmark cancelado."); CloseTransport(); }
            catch (Exception ex) { Log("Benchmark abortado: " + ex.Message); CloseTransport(); }
            finally { Interlocked.Exchange(ref transferRunning, 0); SetTransferUi(false); }
        });
    }

    private void PollPcExplorer()
    {
        if (!pcExplorerSupported || transport is null || Volatile.Read(ref connectInProgress) != 0 ||
            Volatile.Read(ref transferRunning) != 0 || Interlocked.Exchange(ref pcExplorerPolling, 1) != 0) return;
        var generation = connectionGeneration;
        Task.Run(() =>
        {
            var reservedTransfer = false;
            try
            {
                lock (transportGate)
                {
                    if (generation != connectionGeneration || transport is null || Volatile.Read(ref transferRunning) != 0) return;
                    if (!Exchange(Protocol.Frame((Command)0x30, []), out var request) || request.Length == 0) return;
                    using var document = JsonDocument.Parse(request);
                    var root = document.RootElement;
                    var id = root.GetProperty("id").GetInt64();
                    object result; string? file = null;
                    try
                    {
                        if (Volatile.Read(ref pcSharingEnabled) == 0) throw new IOException("Activa Compartir C: en el cliente PC.");
                        var remote = root.GetProperty("path").GetString()!;
                        if (root.GetProperty("op").GetString() == "list")
                            result = new { id, ok = true, data = PcFileBrowser.Listing(remote, root.GetProperty("offset").GetInt32()) };
                        else if (root.GetProperty("op").GetString() == "send")
                        {
                            file = PcFileBrowser.Resolve(remote);
                            using var check = File.Open(file, FileMode.Open, FileAccess.Read, FileShare.Read);
                            if (Interlocked.CompareExchange(ref transferRunning, 1, 0) != 0) throw new IOException("El transporte esta ocupado.");
                            reservedTransfer = true;
                            result = new { id, ok = true };
                        }
                        else throw new IOException("Operacion PC no permitida.");
                    }
                    catch (Exception ex) { result = new { id, ok = false, error = ex.Message }; file = null; }
                    if (!Exchange(Protocol.Frame((Command)0x31, JsonSerializer.SerializeToUtf8Bytes(result)), out _)) return;
                    if (file is not null)
                    {
                        var selected = file;
                        BeginInvoke(() => {
                            if (generation == connectionGeneration && Volatile.Read(ref pcSharingEnabled) != 0 && !IsDisposed) SendQueue(new List<string> { selected }, true);
                            else Interlocked.Exchange(ref transferRunning, 0);
                        });
                        reservedTransfer = false;
                    }
                }
            }
            catch (Exception ex) { Log("Explorar PC: " + ex.Message); }
            finally { if (reservedTransfer) Interlocked.Exchange(ref transferRunning, 0); Interlocked.Exchange(ref pcExplorerPolling, 0); }
        });
    }

    private void SendQueue(List<string>? remoteFiles = null, bool reserved = false)
    {
        if (!reserved && Interlocked.CompareExchange(ref transferRunning, 1, 0) != 0) { Log("Ya hay una transferencia en curso."); return; }
        if (transport is null)
        {
            Log("Aún no hay conexión. Espera a que indique USB/RED · CONECTADO.");
            Interlocked.Exchange(ref transferRunning, 0);
            return;
        }

        var selected = remoteFiles ?? Enumerable.Range(0, sendQueue.Count)
            .Where(i => i < queueView.Items.Count && queueView.GetItemChecked(i))
            .Select(i => sendQueue[i])
            .ToList();
        if (selected.Count == 0)
        {
            Log(sendQueue.Count == 0 ? "Añade al menos un archivo a la bandeja." : "Marca al menos un archivo en la bandeja.");
            Interlocked.Exchange(ref transferRunning, 0);
            return;
        }

        transferCts?.Dispose();
        transferCts = new CancellationTokenSource();
        var token = transferCts.Token;
        SetTransferUi(true);
        Run(() =>
        {
            try
            {
                for (uint i = 0; i < selected.Count; i++)
                {
                    token.ThrowIfCancellationRequested();
                    var file = selected[(int)i];
                    var info = new FileInfo(file);
                    Log($"Preparando {i + 1}/{selected.Count}: {info.Name} ({info.Length} bytes)");
                    Log("Esperando selección en Nintendo Switch...");
                    UsbDebug.Log("TRANSFER", "[BEGIN] transport={0} index={1}/{2} name={3} size={4}", transportKind?.ToString() ?? "?", i + 1, selected.Count, info.Name, info.Length);

                    if (!RequestTransferDecision(info, i, (uint)selected.Count, token, out var decisionPayload))
                        throw new IOException("TRANSFER_BEGIN falló o expiró esperando la decisión de la Switch.");

                    if (!Protocol.TryParseTransferDecision(decisionPayload, out var action, out var targetDirectory, out var decisionError))
                        throw new IOException("Respuesta de decisión inválida: " + decisionError);
                    if ((action == 1 || action == 2) && !new[] { ".nsp", ".xci" }.Contains(info.Extension, StringComparer.OrdinalIgnoreCase))
                        throw new IOException("Este archivo solo se puede copiar a la SD.");

                    UsbDebug.Log("TRANSFER", "[DECISION] index={0} action={1} path={2}", i, action, targetDirectory);
                    if (action == 0)
                    {
                        Log($"Cancelado en la Switch: {info.Name}");
                        continue;
                    }
                    if (action is not (1 or 2 or 3))
                        throw new IOException("Acción desconocida devuelta por la Switch: " + action);

                    var operation = action switch
                    {
                        1 => "Instalar en SD",
                        2 => "Instalar en memoria interna",
                        _ => $"Copiar a {targetDirectory}{(targetDirectory.EndsWith('/') ? "" : "/")}{info.Name}"
                    };
                    Log(operation);

                    // Instalación directa: la Switch pide rangos del NSP/XCI según los necesita
                    // el Installer. No se crea un paquete temporal completo en la SD.
                    if (action is 1 or 2)
                    {
                        using var directInput = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 16 * 1024 * 1024, FileOptions.RandomAccess);
                        if (!Exchange(Protocol.DirectStart(i), out var directStart, 10000, 5000) || Encoding.UTF8.GetString(directStart) != "DIRECT_STARTED")
                            throw new IOException("No se pudo iniciar la instalación directa. Comprueba que PC y NRO usan la misma versión.");
                        Log("Instalación directa MT2.7 activa: prefetch 2x32 MiB, productor y Installer desacoplados.");
                        int directChunk, directWindow;
                        lock (transportGate) { if (!Mtp2NegotiateLocked(out directChunk, out directWindow)) throw new IOException("MT2 no disponible. Actualiza también el NRO o usa el modo compatible."); }
                        Log($"Directo MT2.7: bloque {directChunk / 1024} KiB · techo ventana {directWindow / 1048576.0:0.#} MiB · 64 MiB de prefetch efectivo.");
                        var directTimer = Stopwatch.StartNew(); long served = 0; long lastDirectUi = 0;
                        UpdateTransferProgress(info.Name, 0, info.Length, 0);
                        while (true)
                        {
                            token.ThrowIfCancellationRequested();
                            if (!Exchange(Protocol.DirectPoll(), out var poll, 10000, 5000)) throw new IOException("DIRECT_POLL falló.");
                            var text = Encoding.UTF8.GetString(poll);
                            if (text == "WAIT") { Thread.Sleep(1); continue; }
                            if (text.StartsWith("INSTALL_OK", StringComparison.Ordinal))
                            {
                                var effectiveRate = directTimer.Elapsed.TotalSeconds > 0 ? info.Length / 1048576.0 / directTimer.Elapsed.TotalSeconds : 0;
                                UpdateTransferProgress(info.Name, info.Length, info.Length, effectiveRate);
                                Log($"Instalación directa completada: {info.Name} · {effectiveRate:0.00} MiB/s efectivos");
                                if (text.Length > "INSTALL_OK".Length)
                                {
                                    var parts = text.Split('|', StringSplitOptions.RemoveEmptyEntries);
                                    ulong waitMs = 0, hits = 0, requests = 0, requested = 0;
                                    foreach (var part in parts.Skip(1))
                                    {
                                        var kv = part.Split('=', 2); if (kv.Length != 2) continue;
                                        if (kv[0] == "wait_ms") ulong.TryParse(kv[1], out waitMs);
                                        else if (kv[0] == "hits") ulong.TryParse(kv[1], out hits);
                                        else if (kv[0] == "requests") ulong.TryParse(kv[1], out requests);
                                        else if (kv[0] == "requested") ulong.TryParse(kv[1], out requested);
                                    }
                                    var waitPct = directTimer.Elapsed.TotalMilliseconds > 0 ? waitMs * 100.0 / directTimer.Elapsed.TotalMilliseconds : 0;
                                    Log($"Pipeline MT2.7: espera de datos {waitMs / 1000.0:0.00}s ({waitPct:0.0}%) · cache hits {hits} · peticiones {requests} · servidos {requested / 1048576.0:0.0} MiB.");
                                }
                                break;
                            }
                            if (text == "CANCELLED") throw new OperationCanceledException("Instalación cancelada.", token);
                            if (text.StartsWith("ERR ", StringComparison.Ordinal)) throw new IOException(text);
                            if (!Protocol.TryParseDirectRequest(poll, out var requestId, out var requestOffset, out var requestLength)) throw new IOException("Respuesta DIRECT_POLL desconocida: " + text);
                            lock (transportGate)
                            {
                                if (!Mtp2SendFileRangeLocked(directInput, Mtp2Type.DirectData, requestId, requestOffset, requestLength, directChunk, directWindow, token, true, null, out var directError))
                                    throw new IOException($"DIRECT_DATA MT2 falló en offset {requestOffset}: {directError}");
                            }
                            served += requestLength;
                            if (directTimer.ElapsedMilliseconds - lastDirectUi >= 350)
                            {
                                lastDirectUi = directTimer.ElapsedMilliseconds;
                                var rate = directTimer.Elapsed.TotalSeconds > 0 ? served / 1048576.0 / directTimer.Elapsed.TotalSeconds : 0;
                                UpdateTransferProgress(info.Name, Math.Min(served, info.Length), info.Length, rate);
                            }
                        }
                        continue;
                    }

                    using var input = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 8 * 1024 * 1024, FileOptions.SequentialScan);
                    var timer = Stopwatch.StartNew();
                    long lastUiMs = 0;
                    int transferChunk, transferWindow;
                    lock (transportGate)
                    {
                        if (!Mtp2NegotiateLocked(out transferChunk, out transferWindow)) throw new IOException("MT2 no disponible. Actualiza también el NRO.");
                        Log($"Transferencia MT2.2: bloque {transferChunk / 1024} KiB · ventana {transferWindow / 1048576.0:0.#} MiB.");
                        if (!Mtp2SendFileRangeLocked(input, Mtp2Type.TransferData, i + 1, 0, (ulong)info.Length, transferChunk, transferWindow, token, false,
                            sent =>
                            {
                                if (timer.ElapsedMilliseconds - lastUiMs >= 500 || sent == (ulong)info.Length)
                                {
                                    lastUiMs = timer.ElapsedMilliseconds;
                                    var mibPerSecond = timer.Elapsed.TotalSeconds > 0 ? sent / 1048576.0 / timer.Elapsed.TotalSeconds : 0;
                                    UpdateTransferProgress(info.Name, (long)Math.Min(sent, (ulong)info.Length), info.Length, mibPerSecond);
                                }
                            }, out var mt2Error))
                        {
                            try { Exchange(Protocol.TransferCancel(), out _, 5000, 5000); } catch { }
                            throw new IOException("TRANSFER_DATA MT2 falló: " + mt2Error);
                        }
                    }

                    Log(action == 3 ? "Confirmando copia en la Switch..." : "Transferencia completada. Iniciando instalación en la Switch...");
                    if (!Exchange(Protocol.TransferEnd(i), out var endResult, 60000, 5000))
                        throw new IOException("TRANSFER_END falló.");
                    var endText = Encoding.UTF8.GetString(endResult);
                    if (action == 3)
                    {
                        if (!string.Equals(endText, "COPY_OK", StringComparison.Ordinal))
                            throw new IOException($"La Switch no confirmó COPY_OK: {endText}");
                    }
                    else
                    {
                        if (!string.Equals(endText, "INSTALL_STARTED", StringComparison.Ordinal))
                            throw new IOException($"La Switch no inició la instalación: {endText}");
                        Log("Instalación iniciada. Puedes cancelarla desde el PC o con B en la Switch.");
                        while (true)
                        {
                            token.ThrowIfCancellationRequested();
                            Thread.Sleep(500);
                            if (!Exchange(Protocol.TransferStatus(), out var statusPayload, 10000, 5000))
                                throw new IOException("No se pudo consultar el estado de la instalación.");
                            var status = Encoding.UTF8.GetString(statusPayload);
                            if (status == "INSTALLING") continue;
                            if (status == "INSTALL_OK") break;
                            if (status == "CANCELLED") throw new OperationCanceledException("Instalación cancelada en la Switch.", token);
                            if (status.StartsWith("ERR ", StringComparison.Ordinal)) throw new IOException(status);
                            throw new IOException("Estado de instalación inesperado: " + status);
                        }
                    }

                    timer.Stop();
                    var average = timer.Elapsed.TotalSeconds > 0 ? info.Length / 1048576.0 / timer.Elapsed.TotalSeconds : 0;
                    UpdateTransferProgress(info.Name, info.Length, info.Length, average);
                    Log(action == 3
                        ? $"Copia completada: {info.Name} · {average:0.00} MiB/s"
                        : $"Instalación completada: {info.Name} ({(action == 1 ? "SD" : "memoria interna")})");
                    UsbDebug.Log("TRANSFER", "[END] index={0} action={1} bytes={2} seconds={3:0.000}", i, action, info.Length, timer.Elapsed.TotalSeconds);
                }

                Log("Lote finalizado.");
                BeginInvoke(List);
            }
            catch (OperationCanceledException)
            {
                try { Exchange(Protocol.TransferCancel(), out _, 10000, 5000); } catch { }
                ClearTransferProgress();
                Log("Operación cancelada.");
                UsbDebug.Log("TRANSFER", "[CANCELLED]");
            }
            catch (Exception ex)
            {
                ClearTransferProgress();
                Log("Error en transferencia: " + ex.Message);
                UsbDebug.Log("TRANSFER", "[ERROR] {0}", ex.ToString());
                CloseTransport();
            }
            finally
            {
                SetTransferUi(false);
                Interlocked.Exchange(ref transferRunning, 0);
            }
        });
    }

    private bool RequestTransferDecision(FileInfo file, uint index, uint count, CancellationToken token, out byte[] decision)
    {
        var offer = Protocol.TransferBegin(file.Name, file.Length, index, count);
        if (!asynchronousDecision) return Exchange(offer, out decision, 600000, 5000);
        offer[7] = 0x2C;
        decision = Array.Empty<byte>();
        if (!Exchange(offer, out var accepted) || Encoding.UTF8.GetString(accepted) != "OFFERED") return false;
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < TimeSpan.FromMinutes(10))
        {
            token.ThrowIfCancellationRequested();
            if (!Exchange(Protocol.Frame((Command)0x2D, ReadOnlySpan<byte>.Empty), out decision)) return false;
            if (!decision.AsSpan().SequenceEqual("WAIT"u8)) return true;
            if (token.WaitHandle.WaitOne(100)) token.ThrowIfCancellationRequested();
        }
        Exchange(Protocol.TransferCancel(), out _);
        return false;
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (Volatile.Read(ref transferRunning) != 0) { CancelTransfer(); e.Cancel = true; }
        else { Volatile.Write(ref reconnectEnabled, 0); Interlocked.Increment(ref connectionGeneration); }
        base.OnFormClosing(e);
    }
    protected override void OnFormClosed(FormClosedEventArgs e) { pcExplorerTimer.Stop(); pcExplorerTimer.Dispose(); CloseTransport(); base.OnFormClosed(e); }
    private void CloseTransport() { lock (transportGate) { try { transport?.Dispose(); } catch { } transport = null; transportKind = null; lastAdaptiveWindow = 0; } SetConnectionStatus("ARCHIVOS: SIN CONEXIÓN", Color.Silver); }
    private Button Button(string text, EventHandler click) { return ActionButton(text, click, Color.FromArgb(38,42,44), Color.FromArgb(115,224,235)); }
    private Button ToolButton(string glyph, string description, EventHandler click)
    {
        var button = Button(glyph, click);
        button.Font = new Font("Segoe MDL2 Assets", 14);
        button.AutoSize = false; button.Size = new Size(44, 42); button.Padding = Padding.Empty;
        button.AccessibleName = description; tooltips.SetToolTip(button, description);
        return button;
    }
    private Button ActionButton(string text, EventHandler click, Color background, Color foreground) { var b = new Button { Text = text, AutoSize = true, BackColor = background, ForeColor = foreground, FlatStyle = FlatStyle.Flat, FlatAppearance = { BorderSize = 0 }, Padding = new Padding(12,7,12,7), Margin = new Padding(0,4,8,4), Cursor = Cursors.Hand, Font = new Font("Segoe UI", 9, FontStyle.Bold) }; b.Click += click; var hover = background == Color.FromArgb(181,255,57) ? Color.FromArgb(207,255,120) : Color.FromArgb(30,57,65); b.MouseEnter += (_, _) => { if (b.Enabled) b.BackColor = hover; }; b.MouseLeave += (_, _) => { if (b.Enabled) b.BackColor = background; }; return b; }
    private void SetConnectionStatus(string text, Color color)
    {
        if (InvokeRequired) { BeginInvoke(() => SetConnectionStatus(text, color)); return; }
        transportStatusText = text; transportStatusColor = color; RefreshCombinedStatus();
    }

    private void UpdateRemotePlayStatus(RemotePlayLinkState state, double rttMs)
    {
        if (InvokeRequired) { BeginInvoke(() => UpdateRemotePlayStatus(state, rttMs)); return; }
        (remotePlayStatusText, remotePlayStatusColor) = state switch
        {
            RemotePlayLinkState.Connected => ($"REMOTE PLAY: CONECTADO · RTT {rttMs:F1} ms", Color.FromArgb(181,255,57)),
            RemotePlayLinkState.SendingNoAck => ("REMOTE PLAY: ENVIANDO · SIN ACK", Color.FromArgb(255,220,120)),
            _ => ("REMOTE PLAY: DETENIDO", Color.Silver)
        };
        RefreshCombinedStatus();
    }

    private void RefreshCombinedStatus()
    {
        connectionStatus.Text = $"{transportStatusText}   ·   {remotePlayStatusText}";
        connectionStatus.ForeColor = remotePlayStatusColor != Color.Silver ? remotePlayStatusColor : transportStatusColor;
    }

    private async Task DiscoverSwitchAsync()
    {
        if (Interlocked.Exchange(ref discoveryInProgress, 1) != 0) return;
        try
        {
            SetConnectionStatus("BUSCANDO SWITCH EN LAN", Color.FromArgb(115,224,235));
            var found = await SwitchDiscovery.FindAsync();
            if (found is { } sw)
            {
                consoleIp.Text = sw.Address;
                remotePlay.RefreshEndpoint();
                SetConnectionStatus($"SWITCH {sw.Address} · DESCUBIERTA", Color.FromArgb(181,255,57));
                Log($"Switch magicTupper detectada automáticamente: {sw.Address} ({sw.Banner})");
            }
            else SetConnectionStatus("ARCHIVOS: SIN CONEXIÓN · SWITCH NO DESCUBIERTA", Color.Silver);
        }
        finally { Volatile.Write(ref discoveryInProgress, 0); }
    }
    private void UpdateTransferProgress(string fileName, long sentBytes, long totalBytes, double mibPerSecond)
    {
        if (InvokeRequired) { BeginInvoke(() => UpdateTransferProgress(fileName, sentBytes, totalBytes, mibPerSecond)); return; }
        var safeTotal = Math.Max(0L, totalBytes);
        var safeSent = Math.Clamp(sentBytes, 0L, safeTotal);
        var pending = Math.Max(0L, safeTotal - safeSent);
        var percent = safeTotal > 0 ? 100.0 * safeSent / safeTotal : 0.0;
        var eta = mibPerSecond > 0 ? TimeSpan.FromSeconds(Math.Min(359999, pending / 1048576.0 / mibPerSecond)).ToString(@"hh\:mm\:ss") : "--:--:--";
        transferProgress.Text = $"{percent:0.0}%  |  {mibPerSecond:0.00} MiB/s  |  {safeSent / 1048576.0:0.0} / {safeTotal / 1048576.0:0.0} MiB  |  ETA {eta}  |  {fileName}";
        transferProgress.Visible = true;
    }

    private void ClearTransferProgress()
    {
        if (InvokeRequired) { BeginInvoke(ClearTransferProgress); return; }
        transferProgress.Text = string.Empty;
        transferProgress.Visible = false;
    }

    private void Log(string text) { if (IsDisposed || Disposing) return; if (InvokeRequired) { BeginInvoke(() => Log(text)); return; } if (log.TextLength > 180000) log.Text = log.Text[^90000..]; log.AppendText(text + Environment.NewLine); }
    private void ConnectTcp()
    {
        var ip = consoleIp.Text.Trim();
        if (string.IsNullOrWhiteSpace(ip)) ip = "192.168.1.100";
        consoleIp.Text = ip;
        remotePlay.RefreshEndpoint();
        Log($"Conectando por RED a {ip}:{TcpPort}...");
        RequestConnect(TransportKind.Tcp);
    }

    private void RequestConnect(TransportKind kind)
    {
        if (Volatile.Read(ref transferRunning) != 0) { Log("Termina o cancela la operación antes de cambiar de enlace."); return; }
        var generation = Interlocked.Increment(ref connectionGeneration);
        Volatile.Write(ref reconnectEnabled, 1);
        Run(() => Connect(kind, generation));
    }

    private void DisconnectManual()
    {
        if (Volatile.Read(ref transferRunning) != 0) { CancelTransfer(); return; }
        Volatile.Write(ref reconnectEnabled, 0);
        Interlocked.Increment(ref connectionGeneration);
        SetConnectionStatus("ARCHIVOS: DESCONECTANDO", Color.FromArgb(184,201,204));
        Run(() =>
        {
            CloseTransport();
            SetConnectionStatus("ARCHIVOS: SIN CONEXIÓN", Color.FromArgb(184,201,204));
            Log("Transporte desconectado. Usa 'Conectar USB' o 'Conectar RED' para volver a conectar.");
        });
    }

    private void Connect(TransportKind kind, int generation)
    {
        if (generation != Volatile.Read(ref connectionGeneration) || Volatile.Read(ref reconnectEnabled) == 0) return;
        int attemptNum = Interlocked.Increment(ref connectionAttempt);
        var label = kind == TransportKind.Usb ? "USB" : "RED";
        UsbDebug.Log("CONNECT", "[START] attempt={0} kind={1} generation={2}", attemptNum, label, generation);
        if (Interlocked.Exchange(ref connectInProgress, 1) != 0)
        {
            UsbDebug.Log("CONNECT", "[DEFER] connectInProgress=1");
            Task.Delay(150).ContinueWith(_ =>
            {
                if (!IsDisposed && generation == Volatile.Read(ref connectionGeneration) && Volatile.Read(ref reconnectEnabled) != 0)
                    BeginInvoke(() => Run(() => Connect(kind, generation)));
            });
            return;
        }
        try
        {
            if (generation != Volatile.Read(ref connectionGeneration)) return;
            UsbDebug.Log("CONNECT", "[HELLO] attempt={0}", attemptNum);
            SetConnectionStatus($"ARCHIVOS {label}: CONECTANDO", Color.FromArgb(255,220,120));
            CloseTransport();
            if (generation != Volatile.Read(ref connectionGeneration)) return;
            transport = kind == TransportKind.Usb ? new UsbTransport() : new TcpTransport(consoleIp.Text.Trim(), TcpPort);
            transportKind = kind;
            byte[] response = Array.Empty<byte>();
            var hello = false;
            for (var attempt = 1; attempt <= 3 && !hello; attempt++)
            {
                if (generation != Volatile.Read(ref connectionGeneration)) return;
                if (kind == TransportKind.Tcp) Log("Autoriza la conexion RED en la pantalla de la Switch (A aceptar / B rechazar).");
                hello = Exchange(Protocol.Frame(Command.Hello, ReadOnlySpan<byte>.Empty), out response, kind == TransportKind.Tcp ? 65000 : 10000);
                if (!hello && attempt < 3) Thread.Sleep(350);
            }
            if (!hello) throw new InvalidOperationException("Sin respuesta a HELLO");
            asynchronousDecision = Encoding.UTF8.GetString(response).Contains("ASYNC1", StringComparison.Ordinal);
            pcExplorerSupported = Encoding.UTF8.GetString(response).Contains("PCFS1", StringComparison.Ordinal);
            if (generation != Volatile.Read(ref connectionGeneration)) return;
            UsbDebug.Log("CONNECT", "[HELLO_OK] response={0}", Encoding.UTF8.GetString(response));
            SetConnectionStatus($"ARCHIVOS {label}: CONECTADO", Color.FromArgb(181,255,120));
            lastReconnectMessage = null;
            Log("Conectado (" + label + "): " + Encoding.UTF8.GetString(response));

            UsbDebug.Log("CONNECT", "[LOCK] sending LOCK command");
            if (Exchange(Protocol.Frame(Command.Lock, ReadOnlySpan<byte>.Empty), out var lockResponse))
            {
                UsbDebug.Log("LOCK", "[RESPONSE] exchangeResult=true response={0}", Encoding.UTF8.GetString(lockResponse));
                Log("LOCK: " + Encoding.UTF8.GetString(lockResponse));
            }
            else
            {
                UsbDebug.Log("LOCK", "[RESPONSE] exchangeResult=false (no response)");
                Log("LOCK: sin respuesta");
            }
            Thread.Sleep(250);
            path.Text = "";
            UsbDebug.Log("CONNECT", "[SUCCESS] attempt={0}", attemptNum);
            List();
        }
        catch (Exception ex)
        {
            if (generation != Volatile.Read(ref connectionGeneration) || Volatile.Read(ref reconnectEnabled) == 0) return;
            SetConnectionStatus($"ARCHIVOS {label}: SIN RESPUESTA", Color.FromArgb(255,150,120));
            UsbDebug.Log("CONNECT", "[FAIL] attempt={0} reason={1}", attemptNum, ex.Message);
            var reconnectMessage = $"{kind}: {ex.Message}";
            if (!string.Equals(lastReconnectMessage, reconnectMessage, StringComparison.Ordinal))
            {
                Log($"{label} {TcpPort}: no disponible · Remote Play puede seguir funcionando. {ex.Message}");
                lastReconnectMessage = reconnectMessage;
            }
            CloseTransport();
            ScheduleReconnect(kind, generation, ex.Message);
        }
        finally
        {
            if (generation != Volatile.Read(ref connectionGeneration)) CloseTransport();
            Volatile.Write(ref connectInProgress, 0);
        }
    }

    private void ScheduleReconnect(TransportKind kind, int generation, string reason = "")
    {
        if (IsDisposed || Volatile.Read(ref reconnectEnabled) == 0 || generation != Volatile.Read(ref connectionGeneration)) return;
        UsbDebug.Log("RECONNECT", "[SCHEDULED] kind={0} generation={1} reason={2}", kind, generation, reason);
        Task.Delay(2000).ContinueWith(_ =>
        {
            if (IsDisposed || Volatile.Read(ref reconnectEnabled) == 0 || generation != Volatile.Read(ref connectionGeneration)) return;
            BeginInvoke(() => Run(() => Connect(kind, generation)));
        });
    }
    private void List() => Run(() =>
    {
        var p = path.Text;
        if (Exchange(Protocol.Frame(Command.List, Protocol.SdPath(p)), out var data))
        {
            var text = Encoding.UTF8.GetString(data);
            Log(text);
            BeginInvoke(() =>
            {
                try
                {
                    sdTree.Nodes.Clear();
                    var clean = p.Trim('/');
                    var root = new TreeNode("sdmc:/" + (clean.Length == 0 ? "" : clean)) { Tag = clean, Name = "D" };
                    foreach (var line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
                    {
                        var parts = line.Split(' ', 2);
                        if (parts.Length == 2)
                        {
                            var child = new TreeNode(parts[1]) { Tag = (clean + (clean.Length > 0 ? "/" : "")) + parts[1], Name = parts[0] };
                            if (parts[0] == "D") child.Nodes.Add(new TreeNode());
                            root.Nodes.Add(child);
                        }
                    }
                    sdTree.Nodes.Add(root);
                    root.Expand();
                }
                catch (Exception ex) { Log("Error actualizando árbol SD: " + ex.Message); }
            });
        }
        else Log("LIST falló.");
    });
    private void ReadFile() => ReadFile(sdTree.SelectedNode?.Tag as string ?? path.Text);

    private void ReadFile(string remote, string? destinationOverride = null)
    {
        if (transport is null || Volatile.Read(ref transferRunning) != 0) { Log("Conecta y espera a que termine la operación activa."); return; }
        remote = remote.Replace('\\', '/').Trim('/');
        if (string.IsNullOrWhiteSpace(remote)) { Log("Descarga: selecciona un archivo de la SD."); return; }

        string destination;
        if (string.IsNullOrWhiteSpace(destinationOverride))
        {
            using var dialog = new SaveFileDialog { FileName = Path.GetFileName(remote), Title = "Descargar desde la SD", OverwritePrompt = true };
            if (dialog.ShowDialog() != DialogResult.OK) return;
            destination = dialog.FileName;
        }
        else destination = destinationOverride;

        if (Interlocked.CompareExchange(ref transferRunning, 1, 0) != 0) return;
        transferCts?.Dispose();
        transferCts = new CancellationTokenSource();
        var token = transferCts.Token;
        SetTransferUi(true);
        Log($"Descarga SD -> PC: {remote} -> {destination}");
        Run(() =>
        {
            try
            {
                if (!Exchange(Protocol.Frame(Command.Stat, Protocol.SdPath(remote)), out var stat))
                    throw new IOException($"STAT sin respuesta para '{remote}'.");
                var metadata = Encoding.UTF8.GetString(stat).Trim();
                Log($"STAT {remote}: {metadata}");
                if (metadata.StartsWith("ERR", StringComparison.OrdinalIgnoreCase))
                    throw new IOException($"STAT rechazado para '{remote}': {metadata}");
                if (metadata.StartsWith("D ", StringComparison.Ordinal) || metadata == "D")
                    throw new IOException($"'{remote}' es una carpeta, no un archivo.");
                if (!metadata.StartsWith("F ", StringComparison.Ordinal) || !long.TryParse(metadata.AsSpan(2), out var size) || size < 0)
                    throw new IOException($"STAT no válido para '{remote}': '{metadata}'.");

                var parent = Path.GetDirectoryName(destination);
                if (!string.IsNullOrWhiteSpace(parent)) Directory.CreateDirectory(parent);
                var timer = Stopwatch.StartNew();
                long lastUi = 0;
                if (size == 0)
                {
                    File.WriteAllBytes(destination, Array.Empty<byte>());
                }
                else
                {
                    DownloadFile.Receive(destination, size, (offset, wanted) =>
                    {
                        if (!Exchange(Protocol.Frame(Command.Read, Protocol.ReadRequest(remote, offset, wanted)), out var data, 30000))
                            throw new IOException($"READ sin respuesta en {offset} para '{remote}'.");
                        if (data.Length == 0) throw new IOException($"READ devolvió 0 bytes en {offset} para '{remote}'.");
                        var maybeError = data.Length < 256 ? Encoding.UTF8.GetString(data) : string.Empty;
                        if (maybeError.StartsWith("ERR", StringComparison.OrdinalIgnoreCase))
                            throw new IOException($"READ rechazado en {offset}: {maybeError}");
                        return data;
                    }, token, received =>
                    {
                        if (timer.ElapsedMilliseconds - lastUi < 350 && received != size) return;
                        lastUi = timer.ElapsedMilliseconds;
                        UpdateTransferProgress(Path.GetFileName(remote), received, size, received / 1048576.0 / Math.Max(0.001, timer.Elapsed.TotalSeconds));
                    });
                }
                ClearTransferProgress();
                Log($"Descarga completa: {destination} · {size} bytes");
            }
            catch (OperationCanceledException) { ClearTransferProgress(); Log("Descarga cancelada; temporal eliminado."); }
            catch (Exception ex) { ClearTransferProgress(); Log("Descarga fallida: " + ex.Message); }
            finally { Interlocked.Exchange(ref transferRunning, 0); SetTransferUi(false); }
        });
    }

    private string ResolvePcDropDirectory(Point clientPoint)
    {
        var node = pcTree.GetNodeAt(clientPoint) ?? pcTree.SelectedNode;
        if (node?.Tag is string p)
        {
            if (Directory.Exists(p)) return p;
            if (File.Exists(p)) return Path.GetDirectoryName(p) ?? pcPath;
        }
        return Directory.Exists(pcPath) ? pcPath : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
    }
    private void WriteFile() { using var open = new OpenFileDialog(); if (open.ShowDialog() != DialogResult.OK) return; var destination = Microsoft.VisualBasic.Interaction.InputBox("Destino relativo SD", "WRITE", "switch/" + Path.GetFileName(open.FileName)); if (string.IsNullOrWhiteSpace(destination)) return; Run(() => { var info = new FileInfo(open.FileName); using var input = new FileStream(open.FileName, FileMode.Open, FileAccess.Read, FileShare.Read, 4 * 1024 * 1024, FileOptions.SequentialScan); var buffer = new byte[16 * 1024]; ulong offset = 0; while (offset < (ulong)info.Length) { var count = input.Read(buffer, 0, buffer.Length); if (count == 0 || !Exchange(Protocol.Frame(Command.Write, Protocol.WriteRequest(destination, offset, buffer.AsSpan(0, count))), out var result)) { Log($"WRITE falló en offset {offset}."); return; } offset += (uint)count; Log($"WRITE: {offset}/{info.Length} bytes ({100.0 * offset / info.Length:0.0}%)"); } if (Exchange(Protocol.Frame(Command.Write, Protocol.WriteRequest(destination, offset, ReadOnlySpan<byte>.Empty)), out var committed) && Encoding.UTF8.GetString(committed) == "COMMITTED") Log("WRITE completado."); else Log("WRITE: no se pudo confirmar el archivo."); }); }
    private void Run(Action action)
    {
        Task.Run(() =>
        {
            try { action(); }
            catch (Exception ex) { Log("Error en segundo plano: " + ex.Message); }
        });
    }
    private bool Mtp2ReadLocked(out Mtp2Header header, out byte[] payload, int timeoutMs)
    {
        header = default; payload = Array.Empty<byte>();
        if (transport is null) return false;
        var buffer = mtp2ReadBuffer;
        if (!transport.Read(buffer, 0, timeoutMs, out var count, out var error)) { UsbDebug.Log("MT2", "[READ_FAIL] {0}", error); return false; }
        while (count < Mtp2.HeaderSize)
        {
            if (!transport.Read(buffer, count, timeoutMs, out var n, out error) || n <= 0) { UsbDebug.Log("MT2", "[HEADER_FAIL] {0}", error); return false; }
            count += n;
        }
        var length = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(buffer.AsSpan(16, 4));
        if (length > Mtp2.ChunkSize || Mtp2.HeaderSize + length > buffer.Length) return false;
        var wanted = Mtp2.HeaderSize + (int)length;
        while (count < wanted)
        {
            if (!transport.Read(buffer, count, timeoutMs, out var n, out error) || n <= 0) { UsbDebug.Log("MT2", "[PAYLOAD_FAIL] {0}", error); return false; }
            count += n;
        }
        if (!Mtp2.TryParse(buffer.AsSpan(0, wanted), out header, out var data, out error)) { UsbDebug.Log("MT2", "[PARSE_FAIL] {0}", error); return false; }
        payload = data.ToArray();
        return true;
    }

    private bool Mtp2ExchangeLocked(byte[] frame, out byte[] payload, int readTimeoutMs = 10000, int writeTimeoutMs = 5000)
    {
        payload = Array.Empty<byte>();
        if (transport is null) { UsbDebug.Log("MT2", "[WRITE_FAIL] transporte desconectado"); return false; }
        if (!transport.Write(frame, writeTimeoutMs, out var error)) { UsbDebug.Log("MT2", "[WRITE_FAIL] {0}", error); return false; }
        if (!Mtp2ReadLocked(out var ack, out payload, readTimeoutMs)) return false;
        if ((ack.Flags & Mtp2Flags.Error) != 0 || (byte)ack.Type != (frame[5] | 0x80) ||
            ack.StreamId != BinaryPrimitives.ReadUInt32LittleEndian(frame.AsSpan(8, 4))) return false;
        return true;
    }

    private bool Mtp2NegotiateLocked(out int chunkSize, out int windowSize)
    {
        var desiredChunk = transport is TcpTransport tcp ? tcp.Profile.ChunkSize : Mtp2.ChunkSize;
        var desiredWindow = transportKind == TransportKind.Tcp ? Mtp2.NetworkWindowSize : Mtp2.UsbWindowSize;
        chunkSize = desiredChunk; windowSize = desiredWindow;
        if (transport is null) { UsbDebug.Log("MT2", "[HELLO_WRITE_FAIL] transporte desconectado"); return false; }
        if (!transport.Write(Mtp2.Hello((uint)desiredChunk, (uint)desiredWindow), 5000, out var error)) { UsbDebug.Log("MT2", "[HELLO_WRITE_FAIL] {0}", error); return false; }
        if (!Mtp2ReadLocked(out var header, out var payload, 10000)) return false;
        if ((byte)header.Type != ((byte)Mtp2Type.Hello | 0x80) || payload.Length != 12) return false;
        chunkSize = (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(0, 4));
        windowSize = (int)System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(4, 4));
        if (chunkSize < 64 * 1024 || chunkSize > Mtp2.ChunkSize || windowSize < chunkSize || windowSize > Mtp2.UsbWindowSize) return false;
        return true;
    }

    private bool Mtp2SendFileRangeLocked(FileStream input, Mtp2Type type, uint streamId, ulong sourceOffset, ulong length, int chunkSize, int windowSize, CancellationToken token, bool relativeAck, Action<ulong>? progress, out string error)
    {
        error = string.Empty;
        input.Position = (long)sourceOffset;
        var dataFrame = new byte[Mtp2.HeaderSize + chunkSize];
        var minWindow = transportKind == TransportKind.Tcp ? Math.Max(chunkSize * 2, 1024 * 1024) : Math.Min(windowSize, 8 * 1024 * 1024);
        var learnedUsbWindow = lastAdaptiveWindow > 0 ? lastAdaptiveWindow : 16 * 1024 * 1024;
        var currentWindow = transportKind == TransportKind.Tcp ? Math.Min(windowSize, 2 * 1024 * 1024) : Math.Min(windowSize, Math.Max(minWindow, learnedUsbWindow));
        currentWindow = Math.Max(minWindow, (currentWindow / chunkSize) * chunkSize);
        double bestWindowRate = 0; int badSamples = 0;
        ulong sent = 0; uint seq = 0;
        while (sent < length)
        {
            token.ThrowIfCancellationRequested();
            var windowTarget = Math.Min((ulong)currentWindow, length - sent);
            ulong windowSent = 0;
            var sample = Stopwatch.StartNew();
            while (windowSent < windowTarget)
            {
                var wanted = (int)Math.Min((ulong)chunkSize, windowTarget - windowSent);
                var frame = wanted == chunkSize ? dataFrame : new byte[Mtp2.HeaderSize + wanted];
                var got = 0; while (got < wanted) { var n = input.Read(frame, Mtp2.HeaderSize + got, wanted - got); if (n <= 0) { error = "EOF inesperado"; return false; } got += n; }
                var lastInWindow = windowSent + (ulong)got == windowTarget;
                var lastOverall = sent + windowSent + (ulong)got == length;
                var flags = (lastInWindow ? Mtp2Flags.AckRequired : Mtp2Flags.None) | (lastOverall ? Mtp2Flags.EndOfStream : Mtp2Flags.None);
                var absoluteOffset = sourceOffset + sent + windowSent;
                Mtp2.WriteHeader(frame, type, flags, streamId, seq++, absoluteOffset, got);
                if (transport is null || !transport.Write(frame, 60000, out error)) return false;
                windowSent += (ulong)got;
            }
            sent += windowSent;
            if (!Mtp2ReadLocked(out var ack, out _, 60000)) { error = "ACK MT2 ausente"; return false; }
            if ((ack.Flags & Mtp2Flags.Error) != 0 || ack.AckSequence != seq - 1 || (byte)ack.Type != ((byte)type | 0x80) || ack.StreamId != streamId) { error = "ACK MT2 de tipo/stream incorrecto"; return false; }
            var expectedAck = relativeAck ? sent : sourceOffset + sent;
            if (ack.Offset != expectedAck) { error = $"ACK MT2 desincronizado: {ack.Offset} != {expectedAck}"; return false; }
            sample.Stop();
            var sampleRate = windowSent / 1048576.0 / Math.Max(0.001, sample.Elapsed.TotalSeconds);
            if (bestWindowRate == 0 || sampleRate >= bestWindowRate * 0.98)
            {
                if (sampleRate > bestWindowRate) bestWindowRate = sampleRate;
                badSamples = 0;
                if (currentWindow < windowSize) currentWindow = Math.Min(windowSize, currentWindow + chunkSize);
            }
            else if (sampleRate < bestWindowRate * 0.82)
            {
                if (++badSamples >= 2)
                {
                    currentWindow = Math.Max(minWindow, ((currentWindow / 2) / chunkSize) * chunkSize);
                    badSamples = 0;
                }
            }
            else badSamples = 0;
            lastAdaptiveWindow = currentWindow;
            progress?.Invoke(sent);
        }
        return true;
    }

    private bool Exchange(byte[] frame, out byte[] payload, int readTimeoutMs = 10000, int writeTimeoutMs = 5000)
    {
        lock (transportGate)
        {
            UsbDebug.LogGate("waiting_exchange", frame.Length >= 8 ? frame[7] : (byte)0xFF);
            UsbDebug.LogGate("acquired_exchange", frame.Length >= 8 ? frame[7] : (byte)0xFF);

            payload = Array.Empty<byte>();
            if (transport is null)
            {
                UsbDebug.Log("EXCHANGE", "[BEGIN] transport=null cmd=0x{0:X2} frameLen={1}", frame.Length >= 8 ? frame[7] : 0xFF, frame.Length);
                UsbDebug.LogGate("released_exchange", frame.Length >= 8 ? frame[7] : (byte)0xFF);
                return false;
            }

            var expected = (Command)frame[7];
            UsbDebug.Log("EXCHANGE", "[BEGIN] cmd=0x{0:X2} ({1}) frameLen={2}", frame[7], expected, frame.Length);
            if (frame[7] == (byte)Command.TransferData || frame[7] == (byte)Command.DirectData || frame[7] == (byte)Command.LinkBenchData) UsbDebug.Log("EXCHANGE", "[DATA_FRAME] cmd=0x{0:X2} len={1}", frame[7], frame.Length); else UsbDebug.LogHex("EXCHANGE", frame);

            // WRITE
            if (!transport.Write(frame, writeTimeoutMs, out var writeError))
            {
                UsbDebug.Log("EXCHANGE", "[WRITE_FAIL] cmd=0x{0:X2} error={1}", frame[7], writeError);
                UsbDebug.LogGate("released_exchange", frame[7]);
                return false;
            }
            UsbDebug.Log("EXCHANGE", "[WRITE_OK] cmd=0x{0:X2}", frame[7]);

            // READ header
            var buffer = controlReadBuffer;
            if (!transport.Read(buffer, 0, readTimeoutMs, out var count, out var readError))
            {
                UsbDebug.Log("EXCHANGE", "[READ_FAIL] cmd=0x{0:X2} error={1}", frame[7], readError);
                UsbDebug.LogGate("released_exchange", frame[7]);
                return false;
            }
            UsbDebug.Log("EXCHANGE", "[READ_OK] cmd=0x{0:X2} count={1}", frame[7], count);

            while (count < Protocol.HeaderSize)
            {
                if (!transport.Read(buffer, count, readTimeoutMs, out var received, out readError) || received <= 0) return false;
                count += received;
            }

            var length = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(buffer.AsSpan(8, 4));
            if (length > Protocol.MaxFrame)
            {
                UsbDebug.Log("EXCHANGE", "[PARSE_FAIL] cmd=0x{0:X2} reason=length>MaxFrame length={1}", frame[7], length);
                UsbDebug.LogGate("released_exchange", frame[7]);
                return false;
            }

            // READ remaining payload
            while (count < Protocol.HeaderSize + length)
            {
                if (!transport.Read(buffer, count, readTimeoutMs, out var received, out readError))
                {
                    UsbDebug.Log("EXCHANGE", "[READ_PARTIAL_FAIL] cmd=0x{0:X2} error={1} got={2} expected={3}", frame[7], readError, count, Protocol.HeaderSize + length);
                    UsbDebug.LogGate("released_exchange", frame[7]);
                    return false;
                }
                if (received <= 0)
                {
                    UsbDebug.Log("EXCHANGE", "[PARSE_FAIL] cmd=0x{0:X2} reason=received<=0", frame[7]);
                    UsbDebug.LogGate("released_exchange", frame[7]);
                    return false;
                }
                count += received;
            }

            // PARSE
            if (!Protocol.TryReadResponse(buffer.AsSpan(0, Protocol.HeaderSize + (int)length), expected, out payload, out var error))
            {
                UsbDebug.Log("EXCHANGE", "[PARSE_FAIL] cmd=0x{0:X2} expected=0x{1:X2} reason={2} totalBytes={3}", frame[7], (byte)expected | 0x80, error, count);
                UsbDebug.LogHex("EXCHANGE", buffer, 0, count);
                UsbDebug.LogGate("released_exchange", frame[7]);
                return false;
            }

            UsbDebug.Log("EXCHANGE", "[PARSE_OK] cmd=0x{0:X2} payloadLen={1}", frame[7], payload.Length);
            UsbDebug.LogGate("released_exchange", frame[7]);
            return true;
        }
    }
}
