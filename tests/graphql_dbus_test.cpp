#include "sutra/graphql/executor.hpp"
#include "sutra/spawn.hpp"

#ifdef SUTRA_HAVE_DBUS
#include "sutra/graphql/dbus_source.hpp"
#endif

#include <gtest/gtest.h>

using namespace sutra;
using namespace sutra::graphql;
using nlohmann::json;

#ifdef SUTRA_HAVE_DBUS

namespace
{

constexpr auto sensorIface = "xyz.openbmc_project.Sensor.Value";
constexpr auto statusIface = "xyz.openbmc_project.State.Decorator."
                             "OperationalStatus";
constexpr auto sensorRoot = "/xyz/openbmc_project/sensors";

/** Canned mapper and property answers. */
class FakeBackend : public DbusBackend
{
  public:
    Task<Result<DbusSubTree>> getSubTree(
        std::string path, int, std::vector<std::string> interfaces) override
    {
        subTreeCalls.push_back(path);
        if (path != sensorRoot)
        {
            co_return DbusSubTree{};
        }
        DbusSubTree tree;
        for (const char* name : {"temperature/cpu", "fan_tach/fan_1"})
        {
            auto& services = tree[std::string(sensorRoot) + "/" + name];
            auto& list = services["xyz.test.Sensors"];
            for (const auto& iface : interfaces)
            {
                list.push_back(iface);
            }
        }
        co_return tree;
    }

    Task<Result<DbusPropertyMap>> getAll(std::string service, std::string path,
                                         std::string interface) override
    {
        if (failGetAll)
        {
            co_return std::unexpected(std::string("denied"));
        }
        EXPECT_EQ(service, "xyz.test.Sensors");
        DbusPropertyMap props;
        const bool isFan = path.find("fan_tach") != std::string::npos;
        if (interface == sensorIface)
        {
            props["Value"] = isFan ? 3200.0 : 41.5;
            props["Unit"] = std::string(
                isFan ? "xyz.openbmc_project.Sensor.Value.Unit.RPMS"
                      : "xyz.openbmc_project.Sensor.Value.Unit.DegreesC");
        }
        else if (interface == statusIface)
        {
            props["Functional"] = !isFan;
        }
        co_return props;
    }

    bool failGetAll = false;
    std::vector<std::string> subTreeCalls;
};

json sensorConfig(bool withId)
{
    json config = {
        {"subtree", sensorRoot},
        {"interfaces", {sensorIface}},
        {"fields",
         {{"Id", {{"from", "path"}, {"transform", "sensorId"}}},
          {"Name", {{"from", "path"}, {"transform", "sensorName"}}},
          {"Reading", {{"interface", sensorIface}, {"property", "Value"}}},
          {"Unit",
           {{"interface", sensorIface},
            {"property", "Unit"},
            {"transform", "sensorUnit"}}},
          {"Status/Health",
           {{"interface", statusIface},
            {"property", "Functional"},
            {"transform", "functionalToHealth"}}}}}};
    if (withId)
    {
        config["idKey"] = "Id";
        config["idArg"] = "id";
    }
    return config;
}

Schema makeSchema()
{
    json doc = {
        {"objects",
         {{{"name", "Status"},
           {"fields",
            {{{"name", "Health"},
              {"returnType", "String"},
              {"scalar", true}}}}},
          {{"name", "Sensor"},
           {"fields",
            {{{"name", "Id"}, {"returnType", "String"}, {"scalar", true}},
             {{"name", "Name"}, {"returnType", "String"}, {"scalar", true}},
             {{"name", "Reading"}, {"returnType", "Float"}, {"scalar", true}},
             {{"name", "Unit"}, {"returnType", "String"}, {"scalar", true}},
             {{"name", "Status"}, {"returnType", "Status"}}}}}}},
        {"queries",
         {{{"name", "sensors"},
           {"returnType", "Sensor"},
           {"isList", true},
           {"source", "dbus"},
           {"sourceConfig", sensorConfig(false)}},
          {{"name", "sensor"},
           {"returnType", "Sensor"},
           {"source", "dbus"},
           {"arguments",
            {{{"name", "id"}, {"typeName", "String"}, {"required", true}}}},
           {"sourceConfig", sensorConfig(true)}}}}};
    auto schema = Schema::fromJson(doc);
    EXPECT_TRUE(schema);
    return std::move(*schema);
}

json run(Executor& executor, std::string query)
{
    net::io_context ioc;
    exec::async_scope scope;
    json out;
    auto body = [&]() -> Task<void> {
        out = co_await executor.execute(std::move(query));
    };
    spawnTask(scope, body(), "graphql dbus test");
    ioc.run();
    return out;
}

} // namespace

TEST(GraphqlDbus, ListFromDbus)
{
    auto backend = std::make_shared<FakeBackend>();
    auto registry = std::make_shared<SourceRegistry>();
    registry->add(dbusSourceName, std::make_shared<DbusSource>(backend));
    Schema schema = makeSchema();
    std::vector<std::string> warnings;
    ASSERT_TRUE(applySourceRegistry(schema, *registry, true, warnings));
    Executor executor(std::move(schema), registry);

    json r = run(executor, "{ sensors { Id Name Reading Unit Status { Health "
                           "} } }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    const auto& list = r["data"]["sensors"];
    ASSERT_EQ(list.size(), 2u);
    // std::map order: fan_tach before temperature.
    EXPECT_EQ(list[0]["Id"], "fantach_fan_1");
    EXPECT_EQ(list[0]["Name"], "fan 1");
    EXPECT_EQ(list[0]["Reading"], 3200.0);
    EXPECT_EQ(list[0]["Unit"], "RPM");
    EXPECT_EQ(list[0]["Status"]["Health"], "Critical");
    EXPECT_EQ(list[1]["Id"], "temperature_cpu");
    EXPECT_EQ(list[1]["Unit"], "Cel");
    EXPECT_EQ(list[1]["Status"]["Health"], "OK");
}

TEST(GraphqlDbus, OneObjectById)
{
    auto backend = std::make_shared<FakeBackend>();
    auto registry = std::make_shared<SourceRegistry>();
    registry->add(dbusSourceName, std::make_shared<DbusSource>(backend));
    Executor executor(makeSchema(), registry);

    json r = run(executor, R"({ sensor(id: "temperature_cpu") { Reading } })");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    EXPECT_EQ(r["data"]["sensor"]["Reading"], 41.5);

    r = run(executor, R"({ sensor(id: "nothing") { Reading } })");
    EXPECT_FALSE(r.contains("data"));
    EXPECT_NE(r["errors"][0]["message"].get<std::string>().find("not found"),
              std::string::npos);
}

TEST(GraphqlDbus, GetAllFailureLeavesFieldsNull)
{
    auto backend = std::make_shared<FakeBackend>();
    backend->failGetAll = true;
    auto registry = std::make_shared<SourceRegistry>();
    registry->add(dbusSourceName, std::make_shared<DbusSource>(backend));
    Executor executor(makeSchema(), registry);

    json r = run(executor, "{ sensors { Id Reading } }");
    ASSERT_TRUE(r.contains("data")) << r.dump();
    EXPECT_EQ(r["data"]["sensors"][0]["Id"], "fantach_fan_1");
    EXPECT_TRUE(r["data"]["sensors"][0]["Reading"].is_null());
}

TEST(GraphqlDbus, ConfigValidation)
{
    DbusSource source(std::make_shared<FakeBackend>());
    FieldSpec spec;
    spec.name = "x";
    spec.isList = true;

    spec.sourceConfig = json::object();
    EXPECT_FALSE(source.validate(spec));

    spec.sourceConfig = sensorConfig(false);
    EXPECT_TRUE(source.validate(spec));

    // A non-list field needs idKey/idArg.
    spec.isList = false;
    EXPECT_FALSE(source.validate(spec));
    spec.sourceConfig = sensorConfig(true);
    EXPECT_TRUE(source.validate(spec));

    spec.isList = true;
    json bad = sensorConfig(false);
    bad["subtree"] = "relative/path";
    spec.sourceConfig = bad;
    EXPECT_FALSE(source.validate(spec));

    bad = sensorConfig(false);
    bad["fields"]["Id"]["transform"] = "doesNotExist";
    spec.sourceConfig = bad;
    EXPECT_FALSE(source.validate(spec));

    bad = sensorConfig(false);
    bad["fields"]["Reading"] = {
        {"from", "path"}, {"interface", sensorIface}, {"property", "Value"}};
    spec.sourceConfig = bad;
    EXPECT_FALSE(source.validate(spec));

    // "Status" cannot be both a leaf and the parent of "Status/Health".
    bad = sensorConfig(false);
    bad["fields"]["Status"] = {{"interface", statusIface},
                               {"property", "Functional"}};
    spec.sourceConfig = bad;
    EXPECT_FALSE(source.validate(spec));
}

TEST(GraphqlDbus, Transforms)
{
    EXPECT_EQ(dbus_transform::sensorId("/xyz/openbmc_project/sensors/"
                                       "rotational_position/pos_1"),
              "rotation_pos_1");
    EXPECT_EQ(dbus_transform::leaf("/a/b/c"), "c");
    EXPECT_TRUE(dbus_transform::leaf(5).is_null());
    EXPECT_EQ(dbus_transform::enumLeaf("a.b.Enabled"), "Enabled");
    EXPECT_EQ(dbus_transform::availableToState(false), "UnavailableOffline");
    EXPECT_EQ(dbusValueToJson(DbusValue(std::monostate{})), nullptr);
    EXPECT_TRUE(
        dbusValueToJson(DbusValue(std::numeric_limits<double>::quiet_NaN()))
            .is_null());
}

#else

TEST(GraphqlDbus, SkippedWithoutDbus)
{
    GTEST_SKIP() << "built without SUTRA_HAVE_DBUS";
}

#endif
