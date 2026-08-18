#!/usr/bin/env bash
set -euo pipefail

app="${TEST_SRCDIR}/${TEST_WORKSPACE}/example/app"
log="${TEST_TMPDIR}/app.log"

"${app}" --enable-openapi-docs >"${log}" 2>&1 &
app_pid=$!
trap 'kill "${app_pid}" 2>/dev/null || true; wait "${app_pid}" 2>/dev/null || true' EXIT

python3 - <<'PY'
import socket
import time

for _ in range(100):
    try:
        with socket.create_connection(("127.0.0.1", 8080), timeout=0.1):
            break
    except OSError:
        time.sleep(0.02)
else:
    raise SystemExit("example server did not listen on port 8080")

def request(path):
    chunks = []
    with socket.create_connection(("127.0.0.1", 8080), timeout=1) as sock:
        sock.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
    return b"".join(chunks).decode("iso-8859-1")

homepage = request("/")
assert homepage.startswith("HTTP/1.1 200")
assert 'href="/docs"' in homepage, "homepage must link to API documentation"
assert request("/docs").startswith("HTTP/1.1 200")
PY
