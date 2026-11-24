#!/usr/bin/env python3
"""Tiny fake Redfish service for scripts/demo_graphql.sh (plain HTTP, GET only).

Usage: demo_redfish.py PORT
"""
import http.server
import json
import sys
from urllib.parse import urlsplit

OK = {"State": "Enabled", "Health": "OK"}


def sensor(sid, name, reading, units, top, health="OK"):
    return {
        "@odata.id": f"/redfish/v1/Chassis/chassis/Sensors/{sid}",
        "Id": sid, "Name": name, "Reading": reading, "ReadingUnits": units,
        "ReadingRangeMax": top, "ReadingRangeMin": 0.0,
        "Status": {"State": "Enabled", "Health": health},
    }


SENSORS = [
    sensor("temperature_cpu0", "cpu0", 47.5, "Cel", 105.0),
    sensor("fantach_fan0", "fan0", 5100.0, "RPM", 12000.0),
    sensor("power_psu0_input", "psu0 input", 312.0, "W", 1600.0, "Warning"),
]
SYSTEM = {
    "@odata.id": "/redfish/v1/Systems/system", "Id": "system",
    "Name": "Demo System", "PowerState": "On", "Status": OK,
    "ProcessorSummary": {"Count": 2, "Model": "Demo CPU"},
}
CHASSIS = {
    "@odata.id": "/redfish/v1/Chassis/chassis", "Id": "chassis",
    "Name": "Demo Chassis", "ChassisType": "RackMount",
    "Manufacturer": "Sutra", "Model": "S-1", "Status": OK,
}

TREE = {
    "/redfish/v1": {"Id": "RootService", "Name": "Demo Redfish Service",
                    "RedfishVersion": "1.17.0"},
    "/redfish/v1/Systems": {"Members": [{"@odata.id": SYSTEM["@odata.id"]}]},
    SYSTEM["@odata.id"]: SYSTEM,
    "/redfish/v1/Chassis": {"Members": [{"@odata.id": CHASSIS["@odata.id"]}]},
    CHASSIS["@odata.id"]: CHASSIS,
    "/redfish/v1/Chassis/chassis/Sensors": {"Members": SENSORS},
}
for item in SENSORS:
    TREE[item["@odata.id"]] = item


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = TREE.get(urlsplit(self.path).path)
        data = json.dumps(body if body is not None else {}).encode()
        self.send_response(200 if body is not None else 404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
