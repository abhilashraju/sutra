#!/usr/bin/env python3
"""Tiny session-bus stand-in for the OpenBMC ObjectMapper plus sensors.

Run it under `dbus-run-session` next to `graphql_server --bus session` to see
the D-Bus data source working without a BMC:

    dbus-run-session -- sh -c 'python3 demo_bus.py & graphql_server \
        --bus session --port 8080'
"""
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

SENSORS = {
    "/xyz/openbmc_project/sensors/temperature/cpu0": {
        "Value": 47.5, "MaxValue": 105.0, "MinValue": 0.0,
        "Unit": "xyz.openbmc_project.Sensor.Value.Unit.DegreesC",
        "Functional": True, "Available": True},
    "/xyz/openbmc_project/sensors/fan_tach/fan0": {
        "Value": 5100.0, "MaxValue": 12000.0, "MinValue": 0.0,
        "Unit": "xyz.openbmc_project.Sensor.Value.Unit.RPMS",
        "Functional": True, "Available": True},
    "/xyz/openbmc_project/sensors/power/psu0_input": {
        "Value": 312.0, "MaxValue": 1600.0, "MinValue": 0.0,
        "Unit": "xyz.openbmc_project.Sensor.Value.Unit.Watts",
        "Functional": False, "Available": True},
}
SENSOR_SERVICE = "xyz.sutra.DemoSensors"
VALUE = "xyz.openbmc_project.Sensor.Value"
STATUS = "xyz.openbmc_project.State.Decorator.OperationalStatus"
AVAIL = "xyz.openbmc_project.State.Decorator.Availability"


class Sensor(dbus.service.Object):
    def __init__(self, bus, path, data):
        super().__init__(bus, path)
        self.data = data

    @dbus.service.method("org.freedesktop.DBus.Properties",
                         in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        d = self.data
        if interface == VALUE:
            return {"Value": dbus.Double(d["Value"]),
                    "MaxValue": dbus.Double(d["MaxValue"]),
                    "MinValue": dbus.Double(d["MinValue"]),
                    "Unit": d["Unit"]}
        if interface == STATUS:
            return {"Functional": dbus.Boolean(d["Functional"])}
        if interface == AVAIL:
            return {"Available": dbus.Boolean(d["Available"])}
        return {}


class Mapper(dbus.service.Object):
    @dbus.service.method("xyz.openbmc_project.ObjectMapper",
                         in_signature="sias", out_signature="a{sa{sas}}")
    def GetSubTree(self, root, depth, interfaces):
        result = {}
        for path in SENSORS:
            if path.startswith(root):
                result[path] = {SENSOR_SERVICE: list(interfaces)}
        return result


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    names = [dbus.service.BusName(SENSOR_SERVICE, bus),
             dbus.service.BusName("xyz.openbmc_project.ObjectMapper", bus)]
    objects = [Sensor(bus, p, d) for p, d in SENSORS.items()]
    objects.append(Mapper(bus, "/xyz/openbmc_project/object_mapper"))
    print("demo bus ready", flush=True)
    GLib.MainLoop().run()
    del names, objects


if __name__ == "__main__":
    main()
