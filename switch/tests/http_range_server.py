"""Loopback-only fault injection server for the production C++ HTTP reader."""
import http.server
import re
import subprocess
import sys
import threading

SIZE = 9 * 1024**3
seen = set()

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        assert self.headers.get("Authorization") == "Bearer test-token"
        match = re.fullmatch(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
        assert match
        first, last = map(int, match.groups())
        assert 0 <= first <= last < SIZE and last-first < 1024**2
        if self.path == "/unauthorized":
            self.send_response(401)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        data = bytes((first+i) % 251 for i in range(last-first+1))
        status = 200 if self.path == "/ignore" else 206
        self.send_response(status)
        self.send_header("Content-Range", f"bytes {first+1 if self.path == '/wrong' else first}-{last}/{SIZE}")
        if self.path == "/over":
            data += b"!"
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        key = (self.path, first, last)
        if self.path == "/short" or self.path == "/cut" and key not in seen:
            seen.add(key)
            self.wfile.write(data[:max(1, len(data)//2)])
            self.wfile.flush()
            self.close_connection = True
        else:
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"], check=True, timeout=30)
finally:
    server.shutdown()
    server.server_close()
