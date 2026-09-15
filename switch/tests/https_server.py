"""Local TLS fixture: never uses console credentials or external services."""
import http.server
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(302 if self.path == "/redirect" else 200)
        if self.path == "/redirect":
            self.send_header("Location", "http://127.0.0.1:1/not-followed")
        self.send_header("Content-Length", "0")
        self.end_headers()

with tempfile.TemporaryDirectory(prefix="magictupper-tls-") as directory:
    cert = pathlib.Path(directory) / "ca.pem"
    key = pathlib.Path(directory) / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(cert), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([sys.argv[1], f"https://localhost:{server.server_port}", str(cert)])
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    sys.exit(result.returncode)
