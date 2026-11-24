#pragma once

/**
 * @file sample_data.hpp
 * @brief Static data behind the example's in-memory source.
 */

#include "sutra/graphql/data_source.hpp"

#include <nlohmann/json.hpp>

#include <memory>

namespace example
{

inline nlohmann::json sampleSensors()
{
    return nlohmann::json::parse(R"([
      {"Id": "temperature_cpu0", "Name": "cpu0", "Reading": 47.5,
       "ReadingUnits": "Cel", "ReadingRangeMax": 105.0,
       "ReadingRangeMin": 0.0,
       "Status": {"State": "Enabled", "Health": "OK"}},
      {"Id": "fantach_fan0", "Name": "fan0", "Reading": 5100.0,
       "ReadingUnits": "RPM", "ReadingRangeMax": 12000.0,
       "ReadingRangeMin": 0.0,
       "Status": {"State": "Enabled", "Health": "OK"}},
      {"Id": "power_psu0_input", "Name": "psu0 input", "Reading": 312.0,
       "ReadingUnits": "W", "ReadingRangeMax": 1600.0,
       "ReadingRangeMin": 0.0,
       "Status": {"State": "Enabled", "Health": "Warning"}}
    ])");
}

inline std::shared_ptr<sutra::graphql::MemorySource> makeSampleSource()
{
    using nlohmann::json;
    auto memory = std::make_shared<sutra::graphql::MemorySource>();
    memory->set("serviceRoot", json::parse(R"({
      "Id": "RootService", "Name": "Sutra Root Service",
      "RedfishVersion": "1.17.0"})"));
    json systems = json::parse(R"([
      {"Id": "system", "Name": "Demo System", "PowerState": "On",
       "Status": {"State": "Enabled", "Health": "OK"},
       "ProcessorSummary": {"Count": 2, "Model": "Demo CPU"}}])");
    memory->set("systems", systems);
    memory->set("system", systems);
    memory->set("chassis", json::parse(R"([
      {"Id": "chassis", "Name": "Demo Chassis", "ChassisType": "RackMount",
       "Manufacturer": "Sutra", "Model": "S-1",
       "Status": {"State": "Enabled", "Health": "OK"}}])"));
    memory->set("ethernetInterfaces", json::parse(R"([
      {"Id": "eth0", "MACAddress": "02:00:00:00:00:01", "SpeedMbps": 1000,
       "IPv4Addresses": [{"Address": "192.0.2.10",
                          "SubnetMask": "255.255.255.0",
                          "Gateway": "192.0.2.1"}],
       "Status": {"State": "Enabled", "Health": "OK"}}])"));
    json sensors = sampleSensors();
    memory->set("sensors", sensors);
    memory->set("sensor", sensors);
    return memory;
}

} // namespace example
