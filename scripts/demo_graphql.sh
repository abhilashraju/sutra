#!/bin/sh
# Starts the example GraphQL server (plain HTTP, in-memory data) and queries
# it with curl. Usage: sh scripts/demo_graphql.sh [build-dir]
BUILD=${1:-$HOME/sutra_build/sutra-build}
SERVER="$BUILD/examples/graphql_server/graphql_server"
PORT=${PORT:-28931}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/usr/local/gcc-15/lib64}

"$SERVER" --address 127.0.0.1 --port $PORT --bus none &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
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
echo "--- dbus field dropped without a bus (200 + errors)"
curl -sS -X POST $URL/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ dbusSensors { Id } }"}'
echo
echo "--- bad request (400)"
curl -sS -o /dev/null -w "HTTP %{http_code}\n" -X POST $URL/graphql \
  -H 'Content-Type: application/json' -d 'not json'
