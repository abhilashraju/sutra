#!/bin/sh
# Starts the example GraphQL server (plain HTTP) in front of a fake Redfish
# service (examples/graphql_server/demo_redfish.py) and queries it with curl.
# Usage: sh scripts/demo_graphql.sh [build-dir]
BUILD=${1:-$HOME/sutra_build/sutra-build}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SERVER="$BUILD/examples/graphql_server/graphql_server"
PORT=${PORT:-28931}
REDFISH_PORT=${REDFISH_PORT:-28930}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/local/gcc-15/lib64}

python3 "$HERE/examples/graphql_server/demo_redfish.py" $REDFISH_PORT &
REDFISH_PID=$!
"$SERVER" --address 127.0.0.1 --port $PORT \
    --redfish http://127.0.0.1:$REDFISH_PORT &
PID=$!
trap 'kill $PID $REDFISH_PID 2>/dev/null' EXIT
sleep 1

URL=http://127.0.0.1:$PORT
echo "--- GET /health"
curl -sS $URL/health; echo
echo "--- POST /graphql: aliases + nested objects"
curl -sS -X POST $URL/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ root: serviceRoot { Name RedfishVersion } sys: system(id: \"system\") { PowerState Status { Health } ProcessorSummary { Count } } }"}'
echo
echo "--- POST /graphql: variables + fragment"
curl -sS -X POST $URL/graphql -H 'Content-Type: application/json' \
  -d '{"query":"query S($id: ID) { sensor(id: $id) { ...Basic ReadingRangeMax } } fragment Basic on Sensor { Id Reading ReadingUnits }","variables":{"id":"fantach_fan0"}}'
echo
echo "--- GET /graphql?query= (list)"
curl -sS -G $URL/graphql --data-urlencode 'query={ sensors { Name Reading Status { Health } } }'
echo
echo "--- query error (200 + errors)"
curl -sS -X POST $URL/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ sensors { Bogus } }"}'
echo
echo "--- dbus field dropped without --bus (200 + errors)"
curl -sS -X POST $URL/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ dbusSensors { Id } }"}'
echo
echo "--- bad request (400)"
curl -sS -o /dev/null -w "HTTP %{http_code}\n" -X POST $URL/graphql \
  -H 'Content-Type: application/json' -d 'not json'
