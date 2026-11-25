#pragma once

#include "sutra/graphql/data_source.hpp"
#include "sutra/graphql/executor.hpp"

#include <memory>

namespace graphql_fixture
{

inline nlohmann::json schemaJson()
{
    return nlohmann::json::parse(R"({
  "objects": [
    {"name": "Status", "fields": [
      {"name": "State", "responseKey": "State", "returnType": "String",
       "scalar": true},
      {"name": "Health", "responseKey": "Health", "returnType": "String",
       "scalar": true}
    ]},
    {"name": "Sensor", "fields": [
      {"name": "Id", "responseKey": "Id", "returnType": "String",
       "scalar": true},
      {"name": "Reading", "responseKey": "Reading", "returnType": "Float",
       "scalar": true},
      {"name": "Status", "responseKey": "Status", "returnType": "Status"},
      {"name": "Tags", "responseKey": "Tags", "returnType": "String",
       "isList": false, "scalar": true}
    ]},
    {"name": "Chassis", "fields": [
      {"name": "Name", "responseKey": "Name", "returnType": "String",
       "scalar": true},
      {"name": "Sensors", "responseKey": "Sensors", "returnType": "Sensor",
       "isList": true}
    ]}
  ],
  "queries": [
    {"name": "sensors", "returnType": "Sensor", "isList": true,
     "source": "memory"},
    {"name": "sensor", "returnType": "Sensor", "source": "memory",
     "arguments": [{"name": "id", "typeName": "String", "required": true}]},
    {"name": "chassis", "returnType": "Chassis", "source": "memory"},
    {"name": "version", "returnType": "String", "scalar": true,
     "source": "memory"}
  ],
  "subscriptions": [
    {"name": "sensorUpdates", "returnType": "Sensor", "isList": true,
     "source": "memory"},
    {"name": "sensorStatus", "returnType": "Sensor", "source": "memory",
     "arguments": [{"name": "id", "typeName": "String", "required": true}]},
    {"name": "chassisUpdates", "returnType": "Chassis", "source": "memory"}
  ],
  "dbusWatchers": [
    {
      "dbusObject": "/xyz/openbmc_project/sensors/temp_cpu",
      "dbusInterface": "xyz.openbmc_project.Sensor.Value",
      "dbusProperty": "Value",
      "graphqlFields": ["sensorUpdates", "sensorStatus"]
    },
    {
      "dbusObject": "",
      "dbusInterface": "xyz.openbmc_project.State.Chassis",
      "dbusProperty": "CurrentPowerState",
      "graphqlFields": ["chassisUpdates"]
    }
  ]
})");
}

inline std::shared_ptr<sutra::graphql::MemorySource> makeMemory()
{
    using nlohmann::json;
    auto memory = std::make_shared<sutra::graphql::MemorySource>();
    json sensors = json::array(
        {{{"Id", "temp_cpu"},
          {"Reading", 41.5},
          {"Status", {{"State", "Enabled"}, {"Health", "OK"}}}},
         {{"Id", "fan_1"},
          {"Reading", 3200.0},
          {"Status", {{"State", "Enabled"}, {"Health", "Warning"}}}}});
    memory->set("sensors", sensors);
    memory->set("sensor", sensors);
    memory->set("chassis", {{"Name", "chassis0"}, {"Sensors", sensors}});
    memory->set("version", "1.2.3");
    memory->set("sensorUpdates", sensors);
    memory->set("sensorStatus", sensors);
    memory->set("chassisUpdates", {{"Name", "chassis0"}, {"Sensors", sensors}});
    return memory;
}

inline std::shared_ptr<sutra::graphql::Executor> makeExecutor(
    std::optional<sutra::net::any_io_executor> ioExec = std::nullopt)
{
    using namespace sutra::graphql;
    auto schema = Schema::fromJson(schemaJson());
    auto registry = std::make_shared<SourceRegistry>();
    registry->add("memory", makeMemory());
    std::vector<std::string> warnings;
    auto applied = applySourceRegistry(*schema, *registry, true, warnings);
    if (!applied)
    {
        throw std::runtime_error(applied.error());
    }
    return std::make_shared<Executor>(std::move(*schema), registry,
                                      std::move(ioExec));
}

} // namespace graphql_fixture
