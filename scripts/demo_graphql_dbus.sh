#!/bin/sh
# Runs the example server over HTTPS with the D-Bus source, against a private
# session bus populated by examples/graphql_server/demo_bus.py.
# Usage: dbus-run-session -- sh scripts/demo_graphql_dbus.sh [build-dir]
BUILD=${1:-$HOME/sutra_build/sutra-build}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SERVER="$BUILD/examples/graphql_server/graphql_server"
PORT=${PORT:-28932}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/local/gcc-15/lib64}
TMP=$(mktemp -d)

# The server does not generate certificates: make a throw-away one to pin.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 1 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null

python3 "$HERE/examples/graphql_server/demo_bus.py" &
BUSPID=$!
sleep 1
# --redfish points at a closed local port: this demo only queries dbus* fields.
"$SERVER" --address 127.0.0.1 --port $PORT --bus session \
    --cert "$TMP/cert.pem" --key "$TMP/key.pem" \
    --redfish http://127.0.0.1:9 \
    >"$TMP/server.out" 2>"$TMP/server.err" &
PID=$!
trap 'kill $PID $BUSPID 2>/dev/null; rm -rf "$TMP"' EXIT
sleep 1
# Pin the throw-away certificate with --cacert.
cat "$TMP/server.err"

URL=https://localhost:$PORT
echo "--- dbusSensors over HTTPS (certificate verified with --cacert)"
curl -sS --cacert "$TMP/cert.pem" -X POST $URL/graphql \
  -H 'Content-Type: application/json' \
  -d '{"query":"{ dbusSensors { Id Name Reading ReadingUnits Status { State Health } } }"}'
echo
echo "--- dbusSensor(id:) + alias"
curl -sS --cacert "$TMP/cert.pem" -G $URL/graphql \
  --data-urlencode 'query={ fan: dbusSensor(id: "fantach_fan0") { Reading ReadingUnits ReadingRangeMax } }'
echo
echo "--- unknown id"
curl -sS --cacert "$TMP/cert.pem" -G $URL/graphql \
  --data-urlencode 'query={ dbusSensor(id: "nope") { Reading } }'
echo
echo "--- without --cacert curl must refuse the self-signed certificate"
curl -sS -o /dev/null -w "exit-ok HTTP %{http_code}\n" $URL/health 2>&1 | tail -1
