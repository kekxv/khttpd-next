#!/usr/bin/env bash
set -euo pipefail

app="${TEST_SRCDIR}/${TEST_WORKSPACE}/app"
log="${TEST_TMPDIR}/app.log"

"${app}" --disable-openapi-docs >"${log}" 2>&1 &
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
    with socket.create_connection(("127.0.0.1", 8080), timeout=1) as sock:
        sock.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        return sock.recv(4096).decode("iso-8859-1")

assert request("/").startswith("HTTP/1.1 200"), "business route must remain available"
assert request("/docs").startswith("HTTP/1.1 404"), "docs endpoint must be disabled"
assert request("/openapi.json").startswith("HTTP/1.1 404"), "spec endpoint must be disabled"
PY
