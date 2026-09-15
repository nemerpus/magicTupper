namespace MagicTupperPcClient;

internal sealed class AccountsForm : Form
{
    private readonly TextBox address = new() { Text = "http://192.168.1.100:8765", Dock = DockStyle.Fill };
    private readonly TextBox username = new() { Dock = DockStyle.Fill, MaxLength = 64 };
    private readonly TextBox password = new() { Dock = DockStyle.Fill, UseSystemPasswordChar = true, MaxLength = 256 };
    private readonly TextBox newUsername = new() { Dock = DockStyle.Fill, MaxLength = 64 };
    private readonly TextBox newPassword = new() { Dock = DockStyle.Fill, UseSystemPasswordChar = true, MaxLength = 256 };
    private readonly TextBox confirmation = new() { Dock = DockStyle.Fill, UseSystemPasswordChar = true, MaxLength = 256 };
    private readonly ComboBox role = new() { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly Button login = new() { Text = "Iniciar sesion", AutoSize = true };
    private readonly Button logout = new() { Text = "Cerrar sesion", AutoSize = true };
    private readonly Button refresh = new() { Text = "Actualizar", AutoSize = true };
    private readonly Button create = new() { Text = "Crear usuario", AutoSize = true };
    private readonly Label status = new() { Dock = DockStyle.Fill, AutoEllipsis = true, Text = "Sin sesion", Padding = new Padding(0, 8, 0, 0) };
    private readonly ListView users = new() { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, HideSelection = false, MultiSelect = false };
    private readonly TableLayoutPanel creation = new() { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 5 };
    private readonly CancellationTokenSource lifetime = new();
    private PortalAccounts? portal;
    private bool busy;

    public AccountsForm()
    {
        Text = "MagicTupper - Usuarios";
        ClientSize = new Size(720, 680); MinimumSize = new Size(620, 650);
        StartPosition = FormStartPosition.CenterParent; AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Segoe UI", 10); BackColor = Color.FromArgb(23, 25, 27); ForeColor = Color.White;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5, Padding = new Padding(18) };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var height in new[] { 160, 42, 0, 205, 42 })
            layout.RowStyles.Add(new RowStyle(height == 0 ? SizeType.Percent : SizeType.Absolute, height == 0 ? 100 : height));
        var session = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 4 };
        session.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 150)); session.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        AddField(session, 0, "Portal", address); AddField(session, 1, "Usuario", username); AddField(session, 2, "Contrasena", password);
        var sessionActions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        sessionActions.Controls.AddRange(new Control[] { login, logout }); session.Controls.Add(sessionActions, 1, 3);
        var listActions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
        listActions.Controls.Add(new Label { Text = "USUARIOS", AutoSize = true, Padding = new Padding(0, 8, 18, 0) }); listActions.Controls.Add(refresh);
        users.Columns.Add("Usuario", 330); users.Columns.Add("Rol", 150); users.Columns.Add("Estado", 120);
        users.BackColor = Color.FromArgb(28, 30, 32); users.ForeColor = Color.White;
        creation.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 150)); creation.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        role.Items.AddRange(new object[] { "Estandar", "Administrador" }); role.SelectedIndex = 0;
        AddField(creation, 0, "Nuevo usuario", newUsername); AddField(creation, 1, "Contrasena", newPassword);
        AddField(creation, 2, "Repetir contrasena", confirmation); AddField(creation, 3, "Rol", role);
        creation.Controls.Add(create, 1, 4);
        layout.Controls.Add(session, 0, 0); layout.Controls.Add(listActions, 0, 1); layout.Controls.Add(users, 0, 2);
        layout.Controls.Add(creation, 0, 3); layout.Controls.Add(status, 0, 4); Controls.Add(layout);
        login.Click += async (_, _) => await RunAsync(LoginAsync);
        logout.Click += async (_, _) => await RunAsync(async () =>
        {
            if (portal is not null) await portal.LogoutAsync(lifetime.Token);
            status.Text = "Sesion cerrada";
        });
        refresh.Click += async (_, _) => await RunAsync(RefreshAsync);
        create.Click += async (_, _) => await RunAsync(CreateAsync);
        FormClosed += (_, _) => { lifetime.Cancel(); portal?.Dispose(); password.Clear(); newPassword.Clear(); confirmation.Clear(); };
        UpdateAccess();
    }

    private static void AddField(TableLayoutPanel panel, int row, string label, Control input)
    {
        panel.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        panel.Controls.Add(new Label { Text = label, AutoSize = true, Padding = new Padding(0, 5, 0, 0) }, 0, row);
        input.Margin = new Padding(3, 4, 3, 6); panel.Controls.Add(input, 1, row);
    }

    private void UpdateAccess()
    {
        var signedIn = portal?.IsSignedIn == true;
        var admin = portal?.IsAdmin == true;
        address.Enabled = username.Enabled = password.Enabled = login.Enabled = !busy && !signedIn;
        logout.Enabled = !busy && signedIn; refresh.Enabled = creation.Enabled = !busy && admin;
        if (!admin) { users.Items.Clear(); newPassword.Clear(); confirmation.Clear(); role.SelectedIndex = 0; }
    }

    private async Task RunAsync(Func<Task> operation)
    {
        if (busy) return;
        busy = true; UpdateAccess();
        try { await operation(); }
        catch (OperationCanceledException) { if (!IsDisposed) status.Text = "Operacion cancelada o tiempo de espera agotado"; }
        catch (Exception ex) { if (!IsDisposed) status.Text = ex is System.Net.Http.HttpRequestException ? "No se pudo conectar. Revisa el portal y la confianza del certificado HTTPS." : ex.Message; }
        finally { busy = false; if (!IsDisposed) UpdateAccess(); }
    }

    private async Task LoginAsync()
    {
        var uri = PortalAccounts.Origin(address.Text);
        if (uri.Scheme == "http" && MessageBox.Show(this,
            "Este portal usa HTTP: el usuario, la contrasena y la sesion viajaran sin cifrar. Continuar?",
            "Conexion sin cifrar", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes) return;
        portal?.Dispose(); portal = new PortalAccounts(address.Text);
        try { await portal.LoginAsync(username.Text, password.Text, lifetime.Token); }
        finally { password.Clear(); }
        status.Text = portal.IsAdmin ? $"{portal.Username} / Administrador" : $"{portal.Username} / Estandar: sin permiso para gestionar usuarios";
        if (portal.IsAdmin) await RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        if (portal is null) return;
        var list = await portal.ListAsync(lifetime.Token);
        if (IsDisposed) return;
        users.BeginUpdate(); users.Items.Clear();
        foreach (var user in list) users.Items.Add(new ListViewItem(new[] { user.Username, user.Role == "admin" ? "Administrador" : "Estandar", user.Enabled ? "Activo" : "Desactivado" }));
        users.EndUpdate();
    }

    private async Task CreateAsync()
    {
        if (portal is null || !portal.IsAdmin) return;
        if (newPassword.Text != confirmation.Text) throw new ArgumentException("Las contrasenas no coinciden.");
        var selectedRole = role.SelectedIndex == 1 ? "admin" : "standard";
        if (selectedRole == "admin" && MessageBox.Show(this, $"Crear a {newUsername.Text} como administrador?",
            "Confirmar privilegios", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes) return;
        try { await portal.CreateAsync(newUsername.Text, newPassword.Text, selectedRole, lifetime.Token); }
        finally { newPassword.Clear(); confirmation.Clear(); }
        status.Text = $"Usuario creado: {newUsername.Text}";
        newUsername.Clear(); role.SelectedIndex = 0;
        await RefreshAsync();
    }
}
