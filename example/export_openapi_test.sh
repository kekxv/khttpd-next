#!/usr/bin/env bash
set -euo pipefail

app="${TEST_SRCDIR}/${TEST_WORKSPACE}/example/app"
ready="${TEST_TMPDIR}/port-ready"
output="${TEST_TMPDIR}/example-openapi.json"

python3 - "${ready}" <<'PY' &
import errno
import pathlib
import signal
import socket
import sys

sock = socket.socket()
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    sock.bind(("127.0.0.1", 8080))
    sock.listen(1)
except OSError as error:
    if error.errno != errno.EADDRINUSE:
        raise
    pathlib.Path(sys.argv[1]).write_text("already occupied")
    sys.exit(0)

pathlib.Path(sys.argv[1]).write_text("occupied by test")
signal.pause()
PY
holder_pid=$!
trap 'kill "${holder_pid}" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
  [[ -f "${ready}" ]] && break
  sleep 0.02
done
[[ -f "${ready}" ]]

timeout 10 "${app}" --export-openapi="${output}"

python3 - "${output}" <<'PY'
import json
import pathlib
import sys

document = json.loads(pathlib.Path(sys.argv[1]).read_text())
operation = document["paths"]["/typed/greetings"]["post"]
request = operation["requestBody"]["content"]["application/json"]["schema"]
response = operation["responses"]["200"]["content"]["application/json"]["schema"]
assert request["properties"]["name"]["type"] == "string"
assert response["properties"]["message"]["type"] == "string"
assert "/openapi.json" not in document["paths"]
assert "/docs" not in document["paths"]
PY
