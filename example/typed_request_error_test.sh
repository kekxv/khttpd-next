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

body = b'{"name":42}'
request = (
    b"POST /typed/greetings HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Content-Type: application/json\r\n"
    + f"Content-Length: {len(body)}\r\n".encode()
    + b"Connection: close\r\n\r\n"
    + body
)
with socket.create_connection(("127.0.0.1", 8080), timeout=1) as sock:
    sock.sendall(request)
    chunks = []
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        chunks.append(chunk)

response = b"".join(chunks).decode("iso-8859-1")
assert response.startswith("HTTP/1.1 400"), response
assert response.endswith(
    '{"code":"INVALID_TYPED_REQUEST","message":"Request body must be valid JSON matching the expected schema"}'), response
PY
