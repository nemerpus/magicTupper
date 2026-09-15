using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;

namespace MagicTupperPcClient;

internal sealed record PortalUser(string Id, string Username, string Role, bool Enabled);

internal sealed class PortalAccounts : IDisposable
{
    private readonly HttpClient http;
    private string token = "";
    public string Username { get; private set; } = "";
    public string Role { get; private set; } = "";
    public bool IsAdmin => token.Length > 0 && Role == "admin";
    public bool IsSignedIn => token.Length > 0;

    public static Uri Origin(string address)
    {
        if (!Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri) ||
            (uri.Scheme != "https" && uri.Scheme != "http") || uri.UserInfo.Length != 0 ||
            uri.AbsolutePath != "/" || uri.Query.Length != 0 || uri.Fragment.Length != 0)
            throw new ArgumentException("Introduce el origen del portal: https://dominio o http://IP:puerto.");
        return uri;
    }

    public PortalAccounts(string address, HttpMessageHandler? handler = null)
    {
        var origin = Origin(address);
        http = new HttpClient(handler ?? new HttpClientHandler { AllowAutoRedirect = false, UseCookies = false })
        {
            BaseAddress = origin, Timeout = TimeSpan.FromSeconds(20), MaxResponseContentBufferSize = 256 * 1024
        };
    }

    private async Task<JsonDocument> SendAsync(HttpMethod method, string path, object? body, CancellationToken ct)
    {
        using var request = new HttpRequestMessage(method, path);
        if (token.Length > 0) request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        if (body is not null) request.Content = JsonContent.Create(body);
        using var response = await http.SendAsync(request, ct).ConfigureAwait(false);
        if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden) ClearSession();
        if ((int)response.StatusCode is >= 300 and < 400)
            throw new InvalidOperationException("El portal redirige. Introduce su URL final; no se han reenviado las credenciales.");
        var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false));
        if (!response.IsSuccessStatusCode)
        {
            var message = json.RootElement.TryGetProperty("error", out var error) && error.ValueKind == JsonValueKind.String
                ? error.GetString() : "El portal ha rechazado la operacion.";
            json.Dispose();
            throw new InvalidOperationException(message);
        }
        return json;
    }

    private static PortalUser ReadUser(JsonElement user) => new(user.GetProperty("id").GetString()!,
        user.GetProperty("username").GetString()!, user.GetProperty("role").GetString()!, user.GetProperty("enabled").GetBoolean());

    public async Task LoginAsync(string username, string password, CancellationToken ct)
    {
        ClearSession();
        using var json = await SendAsync(HttpMethod.Post, "/api/login", new { username, password }, ct).ConfigureAwait(false);
        var user = ReadUser(json.RootElement.GetProperty("user"));
        if (!user.Enabled || user.Role is not ("admin" or "standard"))
            throw new InvalidOperationException("La cuenta no tiene un rol valido o esta desactivada.");
        token = json.RootElement.GetProperty("token").GetString() ?? "";
        if (token.Length == 0) throw new InvalidOperationException("El portal no ha devuelto una sesion valida.");
        Username = user.Username;
        Role = user.Role;
    }

    public async Task<IReadOnlyList<PortalUser>> ListAsync(CancellationToken ct)
    {
        RequireAdmin();
        using var json = await SendAsync(HttpMethod.Get, "/api/admin/users", null, ct).ConfigureAwait(false);
        return json.RootElement.GetProperty("users").EnumerateArray().Select(ReadUser).ToArray();
    }

    public async Task CreateAsync(string username, string password, string role, CancellationToken ct)
    {
        RequireAdmin();
        if (username.Length is < 1 or > 64 || username != username.Trim() || username.Any(char.IsControl))
            throw new ArgumentException("Usuario: entre 1 y 64 caracteres, sin espacios en los extremos ni controles.");
        if (password.Length is < 6 or > 256) throw new ArgumentException("Contrasena: entre 6 y 256 caracteres.");
        if (role is not ("standard" or "admin")) throw new ArgumentException("Rol no permitido.");
        using var json = await SendAsync(HttpMethod.Post, "/api/admin/users", new { username, password, role }, ct).ConfigureAwait(false);
    }

    private void RequireAdmin()
    {
        if (!IsAdmin) throw new InvalidOperationException("Solo un administrador puede gestionar usuarios.");
    }

    public async Task LogoutAsync(CancellationToken ct)
    {
        try { if (IsSignedIn) { using var json = await SendAsync(HttpMethod.Post, "/api/logout", new { }, ct).ConfigureAwait(false); } }
        finally { ClearSession(); }
    }

    private void ClearSession() { token = ""; Username = ""; Role = ""; }
    public void Dispose() { ClearSession(); http.Dispose(); }
}
