using System.Net;
using System.Net.Sockets;
using NAudio.Wave;

namespace MagicTupperPcClient;

internal enum RemotePlayLinkState { Stopped, SendingNoAck, Connected }

internal sealed class RemotePlayPanel : UserControl
{
    private readonly Action<RemotePlayLinkState, double>? statusChanged;
    private RemotePlayLinkState linkState = RemotePlayLinkState.Stopped;
    private readonly Func<string> hostProvider;
    private readonly Label ipLabel = new() { AutoSize = true, ForeColor = Color.White, Font = new Font("Segoe UI", 10, FontStyle.Bold) };
    private readonly Label endpointLabel = new() { AutoSize = true, ForeColor = Color.Gainsboro };
    private readonly XInputGamepad xinput = new();
    private readonly LegacyJoystickGamepad joystick = new();
    private readonly SonyHidMotion sonyMotion = new();
    private readonly Label controller = new() { AutoSize = true, ForeColor = Color.White };
    private readonly Label telemetry = new() { AutoSize = true, ForeColor = Color.FromArgb(181,255,57) };
    private readonly Label stateLabel = new() { AutoSize = true, ForeColor = Color.Gainsboro, Font = new Font("Consolas", 10) };
    private readonly Button start = new() { Text = "▶ Mandos", AutoSize = true };
    private readonly Button video = new() { Text = "▶ Vídeo", AutoSize = true };
    private readonly Button fullscreen = new() { Text = "Pantalla completa", AutoSize = true, Enabled = false };
    private readonly Label videoStatus = new() { AutoSize = true, ForeColor = Color.DeepSkyBlue };
    private readonly PictureBox videoView = new() { Dock = DockStyle.Fill, MinimumSize = new Size(320, 180), SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.Black, Cursor = Cursors.Hand };
    private readonly CheckBox[] padEnabled = Enumerable.Range(0,4).Select(i => new CheckBox { Text = $"P{i+1}", Appearance = Appearance.Button, Checked = true, AutoSize = true, TextAlign = ContentAlignment.MiddleCenter }).ToArray();
    private int padMask = 0x0F;
    private readonly CheckBox audioEnabled = new() { Text = "⏸ Audio", Checked = true, Appearance = Appearance.Button, AutoSize = true, ForeColor = Color.White, TextAlign = ContentAlignment.MiddleCenter };
    private readonly ComboBox audioDevice = new() { Width = 300, DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly TrackBar audioVolume = new() { Minimum = 0, Maximum = 100, Value = 80, TickFrequency = 10, Width = 220 };
    private readonly Label audioStatus = new() { AutoSize = true, ForeColor = Color.LightGreen, Text = "AUDIO: preparado para iniciar" };
    private readonly VideoStreamClient videoClient = new();
    private CancellationTokenSource? cts;
    private long sent, acked;
    private long lastAckTick;
    private double rttMs;
    private Bitmap? latestVideoFrame;
    private Form? fullscreenForm;
    private Control? videoOriginalParent;
    private int videoOriginalIndex;
    private DockStyle videoOriginalDock;
    private Size videoOriginalSize;

    public RemotePlayPanel(Func<string> hostProvider, Action<RemotePlayLinkState, double>? statusChanged = null)
    {
        this.hostProvider = hostProvider;
        this.statusChanged = statusChanged;
        BackColor = Color.FromArgb(23,25,27); ForeColor = Color.White; Dock = DockStyle.Fill;

        var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(10), ColumnCount = 1, RowCount = 5, BackColor = Color.FromArgb(19,21,23) };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 26));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));

        var header = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, WrapContents = false };
        header.Controls.Add(new Label { Text = "REMOTE PLAY", AutoSize = true, ForeColor = Color.FromArgb(181,255,57), Font = new Font("Segoe UI", 10, FontStyle.Bold), Padding = new Padding(0,4,12,0) });
        header.Controls.Add(ipLabel);
        header.Controls.Add(endpointLabel);
        root.Controls.Add(header, 0, 0);

        var videoHost = new Panel { Dock = DockStyle.Fill, BackColor = Color.Black, Padding = new Padding(1) };
        videoHost.Controls.Add(videoView);
        root.Controls.Add(videoHost, 0, 1);

        var controls = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, WrapContents = false, Padding = new Padding(0,4,0,0) };
        controls.Controls.Add(video);
        controls.Controls.Add(audioEnabled);
        controls.Controls.Add(start);
        controls.Controls.Add(new Label { Text = "Salida:", AutoSize = true, ForeColor = Color.White, Padding = new Padding(8,7,2,0) });
        audioDevice.Width = 210;
        controls.Controls.Add(audioDevice);
        root.Controls.Add(controls, 0, 2);

        var telemetryRow = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        telemetryRow.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        telemetryRow.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        telemetry.Dock = DockStyle.Fill; telemetry.AutoEllipsis = true;
        videoStatus.Dock = DockStyle.Fill; videoStatus.AutoEllipsis = true;
        telemetryRow.Controls.Add(telemetry, 0, 0);
        telemetryRow.Controls.Add(videoStatus, 0, 1);
        root.Controls.Add(telemetryRow, 0, 3);

        var pads = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, WrapContents = false };
        pads.Controls.Add(new Label { Text = "Mandos:", AutoSize = true, ForeColor = Color.White, Padding = new Padding(0,7,4,0) });
        for (var i = 0; i < padEnabled.Length; i++)
        {
            var slot = i;
            padEnabled[i].CheckedChanged += (_, _) =>
            {
                var bit = 1 << slot;
                if (padEnabled[slot].Checked) Interlocked.Or(ref padMask, bit);
                else Interlocked.And(ref padMask, ~bit);
            };
            pads.Controls.Add(padEnabled[i]);
        }
        pads.Controls.Add(fullscreen);
        root.Controls.Add(pads, 0, 4);
        Controls.Add(root);

        controller.Text = "Solo red · Gamepad 250 Hz · 4 mandos · six-axis Sony HID · Pro Controller virtual";
        telemetry.Text = "REMOTE PLAY: DETENIDO";
        videoStatus.Text = "A/V: DETENIDO · H.264 720p30 + PCM 48 kHz estéreo · UDP/8768";
        stateLabel.Text = "La IP se toma de la conexión de intercambio de archivos.";
        for (int i = 0; i < WaveOut.DeviceCount; i++)
        {
            var caps = WaveOut.GetCapabilities(i);
            audioDevice.Items.Add(new AudioDeviceItem(i, caps.ProductName));
        }
        if (audioDevice.Items.Count > 0) audioDevice.SelectedIndex = 0;
        else { audioDevice.Items.Add(new AudioDeviceItem(-1, "Dispositivo predeterminado de Windows")); audioDevice.SelectedIndex = 0; }
        UpdateEndpointLabel();
        audioVolume.ValueChanged += (_, _) => videoClient.SetAudioVolume(audioVolume.Value / 100f);
        audioEnabled.CheckedChanged += (_, _) =>
        {
            audioEnabled.Text = audioEnabled.Checked ? "⏸ Audio" : "▶ Audio";
            videoClient.SetAudioPlaybackEnabled(audioEnabled.Checked);
            audioStatus.Text = audioEnabled.Checked ? "AUDIO: activado" : "AUDIO: desactivado · reproducción silenciada";
        };
        audioDevice.SelectedIndexChanged += (_, _) =>
        {
            if (audioDevice.SelectedItem is AudioDeviceItem item)
                videoClient.SetAudioDeviceNumber(item.Number);
        };
        start.Click += (_, _) => { if (cts is null) StartSender(); else StopSender(); };
        videoClient.Status += text =>
        {
            if (IsDisposed) return;
            BeginInvoke(() =>
            {
                // Keep A/V telemetry visible. Audio status has its own dedicated line and must not
                // overwrite video/telemetry messages (audio status has its own telemetry line).
                if (text.StartsWith("AUDIO:", StringComparison.OrdinalIgnoreCase) ||
                    text.StartsWith("AUDIO PROBE:", StringComparison.OrdinalIgnoreCase))
                    audioStatus.Text = text;
                else
                    videoStatus.Text = text;
            });
        };
        videoClient.RunningChanged += running => { if (!IsDisposed) BeginInvoke(() => { video.Text = running ? "⏸ Vídeo" : "▶ Vídeo"; fullscreen.Enabled = running; UpdateIpEditState(); }); };
        videoClient.FrameReady += bmp =>
        {
            if (IsDisposed) { bmp.Dispose(); return; }
            var stale = Interlocked.Exchange(ref latestVideoFrame, bmp); stale?.Dispose();
        };
        video.Click += (_, _) => ToggleVideo();
        fullscreen.Click += (_, _) => ToggleFullscreen();
        videoView.Click += (_, _) => ToggleFullscreen();
        Disposed += (_, _) =>
        {
            ExitFullscreen(); videoClient.Stop(); sonyMotion.Dispose(); xinput.SetVibration(0,0,0);
            Interlocked.Exchange(ref latestVideoFrame, null)?.Dispose(); videoView.Image?.Dispose(); StopSender();
        };

        var ui = new System.Windows.Forms.Timer { Interval = 16 };
        long lastTextRefresh = 0;
        ui.Tick += (_, _) =>
        {
            var newest = Interlocked.Exchange(ref latestVideoFrame, null);
            if (newest is not null) { var old = videoView.Image; videoView.Image = newest; old?.Dispose(); }

            var now = Environment.TickCount64;
            if (now - lastTextRefresh >= 100)
            {
                lastTextRefresh = now;
                var tx = Interlocked.Read(ref sent); var ack = Interlocked.Read(ref acked);
                if (cts is null)
                {
                    telemetry.Text = "REMOTE PLAY: DETENIDO";
                }
                else
                {
                    var ackAge = lastAckTick == 0 ? long.MaxValue : now - Interlocked.Read(ref lastAckTick);
                    if (ack > 0 && ackAge <= 1500)
                    {
                        telemetry.Text = $"REMOTE PLAY: CONECTADO · TX {tx} · ACK {ack} · RTT {rttMs:F1} ms";
                        if (linkState != RemotePlayLinkState.Connected) SetLinkState(RemotePlayLinkState.Connected);
                    }
                    else
                    {
                        telemetry.Text = ack == 0
                            ? $"REMOTE PLAY: ESPERANDO RESPUESTA · TX {tx}"
                            : $"REMOTE PLAY: CONEXIÓN PERDIDA · reintentando · TX {tx} · último ACK hace {ackAge / 1000.0:F1}s";
                        if (linkState != RemotePlayLinkState.SendingNoAck) SetLinkState(RemotePlayLinkState.SendingNoAck);
                    }
                }
            }
        };
        ui.Start();
    }

    private string? ValidateHost(bool showMessage = true)
    {
        var host = (hostProvider?.Invoke() ?? string.Empty).Trim();
        ipLabel.Text = string.IsNullOrWhiteSpace(host) ? "IP: sin conexión" : $"IP: {host}";
        if (IPAddress.TryParse(host, out _)) return host;
        if (showMessage) MessageBox.Show("Conecta primero la Switch por RED o deja que magicTupper la descubra para obtener su IP.", "MagicTupper Remote Play", MessageBoxButtons.OK, MessageBoxIcon.Information);
        return null;
    }

    public void RefreshEndpoint()
    {
        UpdateEndpointLabel();
    }

    private void UpdateEndpointLabel()
    {
        var h = (hostProvider?.Invoke() ?? string.Empty).Trim();
        ipLabel.Text = string.IsNullOrWhiteSpace(h) ? "IP: sin conexión" : $"IP: {h}";
        endpointLabel.Text = string.IsNullOrWhiteSpace(h) ? "" : $" · {h}:{VideoStreamClient.Port}/UDP";
    }

    private void UpdateIpEditState() => UpdateEndpointLabel();

    private void StartSender()
    {
        if (cts is not null) return;
        var host = ValidateHost(); if (host is null || !IPAddress.TryParse(host, out var ip)) return;

        Interlocked.Exchange(ref sent, 0); Interlocked.Exchange(ref acked, 0); Interlocked.Exchange(ref lastAckTick, 0); rttMs = 0;
        cts = new CancellationTokenSource(); start.Text = "⏸ Mandos"; UpdateIpEditState();
        SetLinkState(RemotePlayLinkState.SendingNoAck);
        var token = cts.Token;
        _ = Task.Run(async () =>
        {
            using var udp = new UdpClient(ip.AddressFamily);
            udp.Connect(new IPEndPoint(ip, RemotePlayProtocol.InputPort));
            uint seq = 0;

            var ackTask = Task.Run(async () =>
            {
                while (!token.IsCancellationRequested)
                {
                    try
                    {
                        var result = await udp.ReceiveAsync(token);
                        if (RemotePlayProtocol.TryParseAck(result.Buffer, out _, out var echoed, out _))
                        {
                            Interlocked.Increment(ref acked);
                            Interlocked.Exchange(ref lastAckTick, Environment.TickCount64);
                            rttMs = Math.Max(0, (RemotePlayProtocol.MonotonicUs() - echoed) / 1000.0);
                        }
                    }
                    catch (OperationCanceledException) { break; }
                    catch (SocketException) { if (!token.IsCancellationRequested) { SetLinkState(RemotePlayLinkState.SendingNoAck); try { await Task.Delay(100, token); } catch { break; } } }
                    catch { if (!token.IsCancellationRequested) { try { await Task.Delay(100, token); } catch { break; } } }
                }
            }, token);

            long lastControllerUi = 0;
            while (!token.IsCancellationRequested)
            {
                var lines = new List<string>(4);
                for (byte player = 0; player < 4; player++)
                {
                    var timestamp = RemotePlayProtocol.MonotonicUs();
                    var slotEnabled = (Volatile.Read(ref padMask) & (1 << player)) != 0;
                    var backend = "";
                    var pad = MtGamepadState.Neutral;
                    var connected = slotEnabled && xinput.TryRead(player, out pad);
                    if (connected) backend = "XInput";
                    else if (slotEnabled && joystick.TryReadSlot(player, out pad, out var hidName)) { connected = true; backend = hidName; }
                    else pad = MtGamepadState.Neutral;

                    var motion = MtMotionState.None; var motionTag = "";
                    if (connected && sonyMotion.TryRead(player, out var sm, out _)) { motion = sm; motionTag = " · GYRO"; }
                    if ((player == 0 && slotEnabled) || connected)
                    {
                        var packet = RemotePlayProtocol.Input(++seq, timestamp, pad, player, motion);
                        try { await udp.SendAsync(packet, token); Interlocked.Increment(ref sent); }
                        catch (OperationCanceledException) { break; }
                        catch (SocketException) { SetLinkState(RemotePlayLinkState.SendingNoAck); }
                        catch { SetLinkState(RemotePlayLinkState.SendingNoAck); }
                    }
                    if (connected)
                        lines.Add($"P{player + 1} · {backend}{motionTag} · {pad.Buttons} · LX {pad.LeftX} LY {pad.LeftY} RX {pad.RightX} RY {pad.RightY}");
                }

                var nowUi = Environment.TickCount64;
                if (!IsDisposed && nowUi - lastControllerUi >= 100)
                {
                    lastControllerUi = nowUi;
                    var snapshot = lines.Count > 0 ? string.Join("\n", lines) : "Mando no conectado (XInput/HID; P1 mantiene el enlace en neutro)";
                    try { BeginInvoke(() => stateLabel.Text = snapshot); } catch { }
                }
                try { await Task.Delay(4, token); } catch (OperationCanceledException) { break; }
            }
            try { await ackTask; } catch { }
            for (byte player = 0; player < 4; player++)
                for (var i = 0; i < 2; i++)
                    try { await udp.SendAsync(RemotePlayProtocol.Input(++seq, RemotePlayProtocol.MonotonicUs(), MtGamepadState.Neutral, player, MtMotionState.None)); } catch { }
        }, token);
    }

    private void ToggleVideo()
    {
        if (!videoClient.Running)
        {
            var item = audioDevice.SelectedItem as AudioDeviceItem;
            videoClient.AudioEnabled = true;
            videoClient.SetAudioPlaybackEnabled(audioEnabled.Checked);
            videoClient.AudioDeviceNumber = item?.Number ?? -1;
            videoClient.AudioVolume = audioVolume.Value / 100f;
            audioStatus.Text = audioEnabled.Checked ? "AUDIO: iniciando…" : "AUDIO: desactivado";
        }

        if (videoClient.Running)
        {
            ExitFullscreen(); videoClient.Stop();
            return;
        }
        var host = ValidateHost(); if (host is null) return;
        try
        {
            videoClient.Start(host);
            video.Text = "⏸ Vídeo"; fullscreen.Enabled = true; UpdateIpEditState();
        }
        catch (Exception ex) { MessageBox.Show(ex.Message, "MagicTupper Video", MessageBoxButtons.OK, MessageBoxIcon.Information); }
    }

    private void ToggleFullscreen()
    {
        if (!videoClient.Running) return;
        if (fullscreenForm is not null) { ExitFullscreen(); return; }
        videoOriginalParent = videoView.Parent;
        videoOriginalIndex = videoOriginalParent?.Controls.GetChildIndex(videoView) ?? 0;
        videoOriginalDock = videoView.Dock; videoOriginalSize = videoView.Size;
        var fs = new Form { BackColor = Color.Black, FormBorderStyle = FormBorderStyle.None, WindowState = FormWindowState.Maximized, StartPosition = FormStartPosition.Manual, KeyPreview = true, Text = "MagicTupper · Remote Play fullscreen" };
        fullscreenForm = fs;
        fs.KeyDown += (_, e) => { if (e.KeyCode == Keys.Escape || (e.Alt && e.KeyCode == Keys.Enter)) { ExitFullscreen(); e.Handled = true; e.SuppressKeyPress = true; } };
        fs.FormClosing += (_, e) => { if (fullscreenForm == fs) { e.Cancel = true; ExitFullscreen(); } };
        fs.Controls.Add(videoView); videoView.Dock = DockStyle.Fill; videoView.SizeMode = PictureBoxSizeMode.Zoom;
        fullscreen.Text = "Salir de pantalla completa";
        var owner = FindForm(); if (owner is not null) fs.Show(owner); else fs.Show(); fs.Activate();
    }

    private void ExitFullscreen()
    {
        var fs = fullscreenForm; if (fs is null) return; fullscreenForm = null;
        fs.Controls.Remove(videoView);
        if (videoOriginalParent is not null && !videoOriginalParent.IsDisposed)
        {
            videoOriginalParent.Controls.Add(videoView);
            try { videoOriginalParent.Controls.SetChildIndex(videoView, Math.Min(videoOriginalIndex, videoOriginalParent.Controls.Count - 1)); } catch { }
        }
        videoView.Dock = videoOriginalDock; videoView.Size = videoOriginalSize; fullscreen.Text = "Pantalla completa";
        try { fs.Dispose(); } catch { }
    }

    private void StopSender()
    {
        var old = cts; if (old is null) return; cts = null; old.Cancel(); old.Dispose(); start.Text = "▶ Mandos";
        SetLinkState(RemotePlayLinkState.Stopped); UpdateIpEditState();
    }

    private void SetLinkState(RemotePlayLinkState state)
    {
        linkState = state;
        try { statusChanged?.Invoke(state, rttMs); } catch { }
    }
    private sealed record AudioDeviceItem(int Number, string Name) { public override string ToString() => Name; }
}
