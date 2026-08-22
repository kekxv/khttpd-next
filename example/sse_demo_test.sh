#!/usr/bin/env bash
set -euo pipefail

app="${TEST_SRCDIR}/${TEST_WORKSPACE}/app"
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

with socket.create_connection(("127.0.0.1", 8080), timeout=1) as sock:
    sock.settimeout(2)
    sock.sendall(b"GET /events HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n")
    response = bytearray()
    while b"event: welcome" not in response or b"event: tick" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise AssertionError("SSE connection closed before the demo events arrived")
        response.extend(chunk)

wire = bytes(response)
assert wire.startswith(b"HTTP/1.1 200"), wire
assert b"Content-Type: text/event-stream" in wire, wire
assert b"event: welcome" in wire and b"Connected to the khttpd SSE demo" in wire, wire
assert b"event: tick" in wire and b'"sequence":1' in wire, wire
PY
