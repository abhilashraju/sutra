/**
 * Needs a session bus: meson runs it through `dbus-run-session`.
 *
 * Two connections live on one io_context: a test service (properties,
 * methods) plus an ObjectMapper stand-in, and the client under test.
 */
#include "sutra/dbus.hpp"
#include "sutra/spawn.hpp"

#include <gtest/gtest.h>

using namespace sutra;

namespace
{

constexpr auto testService = "com.sutra.TestService";
constexpr auto testPath = "/com/sutra/test/obj1";
constexpr auto testInterface = "com.sutra.Test";

/** Connects to the session bus; owns `name` when one is given. */
std::shared_ptr<sdbusplus::asio::connection> connect(net::io_context& ioc,
                                                     const char* name = nullptr)
{
    auto conn = makeConnection(ioc, true);
    EXPECT_TRUE(conn);
    if (!conn)
    {
        std::abort();
    }
    if (name != nullptr)
    {
        (*conn)->request_name(name);
    }
    return *conn;
}

/** The service the tests talk to. */
class TestService
{
  public:
    explicit TestService(net::io_context& ioc) :
        conn(connect(ioc, testService)), server(conn)
    {
        iface = server.add_interface(testPath, testInterface);
        iface->register_property(
            "Name", std::string("alpha"),
            sdbusplus::asio::PropertyPermission::readWrite);
        iface->register_property(
            "Count", uint32_t(7),
            sdbusplus::asio::PropertyPermission::readWrite);
        iface->register_property("Enabled", true);
        iface->register_method("Add", [](int32_t a, int32_t b) {
            return a + b;
        });
        iface->register_method("Describe", [](std::string text) {
            return std::make_tuple("<" + text + ">",
                                   static_cast<uint32_t>(text.size()));
        });
        iface->register_method("Ping", []() {});
        iface->register_method("Fail", []() {
            throw std::runtime_error("deliberate failure");
        });
        iface->initialize();
    }

  private:
    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::asio::object_server server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
};

/** Just enough of xyz.openbmc_project.ObjectMapper for the tests. */
class MapperStub
{
  public:
    explicit MapperStub(net::io_context& ioc) :
        conn(connect(ioc, objectMapperService)), server(conn)
    {
        iface = server.add_interface(objectMapperPath, objectMapperInterface);
        iface->register_method(
            "GetSubTree", [](const std::string& path, int32_t,
                             const std::vector<std::string>& interfaces) {
                SubTreeMap tree;
                if (matches(path, interfaces))
                {
                    tree[testPath][testService] = {testInterface};
                }
                return tree;
            });
        iface->register_method("GetSubTreePaths",
                               [](const std::string& path, int32_t,
                                  const std::vector<std::string>& interfaces) {
                                   std::vector<std::string> paths;
                                   if (matches(path, interfaces))
                                   {
                                       paths.emplace_back(testPath);
                                   }
                                   return paths;
                               });
        iface->register_method(
            "GetObject",
            [](const std::string& path, const std::vector<std::string>&) {
                if (path != testPath)
                {
                    throw std::runtime_error("no such object");
                }
                ObjectMap result;
                result[testService] = {testInterface};
                return result;
            });
        iface->initialize();
    }

  private:
    static bool matches(const std::string& root,
                        const std::vector<std::string>& interfaces)
    {
        const std::string path = testPath;
        return path.starts_with(root) &&
               (interfaces.empty() ||
                std::ranges::find(interfaces, testInterface) !=
                    interfaces.end());
    }

    std::shared_ptr<sdbusplus::asio::connection> conn;
    sdbusplus::asio::object_server server;
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
};

/** Runs `body` as a task against a fresh service + client connection. */
template <class Body>
void runWithBus(Body body)
{
    net::io_context ioc;
    TestService service(ioc);
    MapperStub mapper(ioc);
    auto clientPtr = connect(ioc);
    auto& client = *clientPtr;

    exec::async_scope scope;
    bool failed = false;
    auto wrapper = [&]() -> Task<void> {
        try
        {
            co_await body(client);
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << "test body failed: " << e.what();
            failed = true;
        }
        ioc.stop();
    };
    spawnTask(scope, wrapper(), "dbus test");
    ioc.run();
    EXPECT_FALSE(failed);
}

/** The error of a failed call; a default (success) code when it worked. */
template <class Awaited>
Task<error_code> failureOf(Awaited&& call)
{
    auto result = co_await std::forward<Awaited>(call);
    if (!result)
    {
        co_return result.error();
    }
    co_return error_code{};
}

} // namespace

TEST(DBus, GetAndSetProperty)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        auto name = co_await getProperty<std::string>(
            bus, testService, testPath, testInterface, "Name");
        EXPECT_TRUE(name);
        EXPECT_EQ(name.value_or(""), "alpha");

        auto set = co_await setProperty<std::string>(
            bus, testService, testPath, testInterface, "Name", "beta");
        EXPECT_TRUE(set);
        name = co_await getProperty<std::string>(bus, testService, testPath,
                                                 testInterface, "Name");
        EXPECT_EQ(name.value_or(""), "beta");

        set = co_await setProperty<uint32_t>(bus, testService, testPath,
                                             testInterface, "Count", 99);
        EXPECT_TRUE(set);
        auto count = co_await getProperty<uint32_t>(bus, testService, testPath,
                                                    testInterface, "Count");
        EXPECT_EQ(count.value_or(0), 99u);
    });
}

TEST(DBus, PropertyErrors)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        // Wrong type requested.
        auto ec = co_await failureOf(getProperty<uint32_t>(
            bus, testService, testPath, testInterface, "Name"));
        EXPECT_EQ(ec, net::error::invalid_argument);

        // Unknown property, object and service.
        ec = co_await failureOf(getProperty<bool>(bus, testService, testPath,
                                                  testInterface, "Missing"));
        EXPECT_TRUE(ec);
        ec = co_await failureOf(
            getProperty<bool>(bus, testService, "/com/sutra/test/none",
                              testInterface, "Enabled"));
        EXPECT_TRUE(ec);
        ec = co_await failureOf(
            getProperty<bool>(bus, "com.sutra.NoSuchService", testPath,
                              testInterface, "Enabled"));
        EXPECT_TRUE(ec);

        // Read-only property cannot be set.
        ec = co_await failureOf(setProperty<bool>(
            bus, testService, testPath, testInterface, "Enabled", false));
        EXPECT_TRUE(ec);
    });
}

TEST(DBus, GetAllProperties)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        auto all = co_await getAllProperties(bus, testService, testPath,
                                             testInterface);
        EXPECT_TRUE(all);
        if (!all)
        {
            co_return;
        }
        const auto& props = *all;
        EXPECT_EQ(props.size(), 3u);
        auto values = getPropertiesFromMap<std::string, uint32_t, bool>(
            props, "Name", "Count", "Enabled");
        EXPECT_TRUE(values);
        if (values)
        {
            auto [name, count, enabled] = *values;
            EXPECT_EQ(name, "alpha");
            EXPECT_EQ(count, 7u);
            EXPECT_TRUE(enabled);
        }

        auto missing = getPropertyFromMap<bool>(props, "Missing");
        EXPECT_FALSE(missing);
        EXPECT_EQ(missing.error(), net::error::not_found);
        auto wrongType = getPropertyFromMap<bool>(props, "Name");
        EXPECT_FALSE(wrongType);
        EXPECT_EQ(wrongType.error(), net::error::invalid_argument);

        auto mixed =
            getPropertiesFromMap<std::string, bool>(props, "Name", "Missing");
        EXPECT_FALSE(mixed);
        EXPECT_EQ(mixed.error(), net::error::not_found);
    });
}

TEST(DBus, MethodCalls)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        auto sum = co_await dbusMethodCall<int32_t>(
            bus, testService, testPath, testInterface, "Add", int32_t(2),
            int32_t(40));
        EXPECT_TRUE(sum);
        EXPECT_EQ(sum.value_or(-1), 42);

        // The service returns a std::tuple, which sdbusplus sends as one
        // D-Bus struct.
        auto both = co_await dbusMethodCall<std::tuple<std::string, uint32_t>>(
            bus, testService, testPath, testInterface, "Describe",
            std::string("abc"));
        EXPECT_TRUE(both);
        if (both)
        {
            EXPECT_EQ(std::get<0>(*both), "<abc>");
            EXPECT_EQ(std::get<1>(*both), 3u);
        }

        auto ping = co_await dbusMethodCall<>(bus, testService, testPath,
                                              testInterface, "Ping");
        EXPECT_TRUE(ping);

        auto ec = co_await failureOf(dbusMethodCall<>(
            bus, testService, testPath, testInterface, "Fail"));
        EXPECT_TRUE(ec);

        // Wrong argument types are reported, not crashed on.
        ec = co_await failureOf(
            dbusMethodCall<int32_t>(bus, testService, testPath, testInterface,
                                    "Add", std::string("x"), std::string("y")));
        EXPECT_TRUE(ec);
    });
}

TEST(DBus, ConcurrentCalls)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        std::vector<int32_t> results(8, -1);
        exec::async_scope scope;
        auto worker = [&](int index) -> Task<void> {
            auto sum = co_await dbusMethodCall<int32_t>(
                bus, testService, testPath, testInterface, "Add",
                int32_t(index), int32_t(100));
            results[index] = sum.value_or(-1);
        };
        for (int i = 0; i < 8; ++i)
        {
            spawnTask(scope, worker(i), "worker");
        }
        co_await scope.on_empty();
        for (int i = 0; i < 8; ++i)
        {
            EXPECT_EQ(results[i], 100 + i);
        }
    });
}

TEST(DBus, ObjectMapperCalls)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        auto tree = co_await getSubTree(bus, "/com/sutra", 0, {testInterface});
        EXPECT_TRUE(tree);
        if (!tree || tree->empty())
        {
            ADD_FAILURE() << "expected one subtree entry";
            co_return;
        }
        EXPECT_EQ(tree->size(), 1u);
        EXPECT_EQ(tree->begin()->first, testPath);
        EXPECT_EQ(tree->begin()->second.begin()->first, testService);

        tree = co_await getSubTree(bus, "/com/sutra", 0, {"no.such.Iface"});
        EXPECT_TRUE(tree);
        EXPECT_TRUE(tree && tree->empty());

        auto paths = co_await getSubTreePaths(bus, "/", 0);
        EXPECT_TRUE(paths);
        EXPECT_EQ(paths.value_or(std::vector<std::string>{}),
                  std::vector<std::string>{testPath});

        auto objects = co_await getObjects(bus, testPath);
        EXPECT_TRUE(objects);
        if (!objects || objects->empty())
        {
            ADD_FAILURE() << "expected one object entry";
            co_return;
        }
        EXPECT_EQ(objects->size(), 1u);
        EXPECT_EQ(objects->begin()->first, testService);

        auto ec = co_await failureOf(getObjects(bus, "/not/there"));
        EXPECT_TRUE(ec);

        // The stub does not implement the rest: that is a clean error.
        ec = co_await failureOf(getAncestors(bus, testPath));
        EXPECT_TRUE(ec);
    });
}

TEST(DBus, ManagedObjectsAndIntrospection)
{
    runWithBus([](sdbusplus::asio::connection& bus) -> Task<void> {
        auto managed = co_await getManagedObjects(bus, testService, "/");
        EXPECT_TRUE(managed);
        if (!managed)
        {
            co_return;
        }
        auto it = managed->find(ObjectPath(testPath));
        EXPECT_NE(it, managed->end());
        if (it == managed->end())
        {
            co_return;
        }
        const auto& props = it->second.at(testInterface);
        auto name = getPropertyFromMap<std::string>(props, "Name");
        EXPECT_EQ(name.value_or(""), "alpha");

        auto xml = co_await introspect(bus, testService, testPath);
        EXPECT_TRUE(xml);
        if (!xml)
        {
            co_return;
        }
        EXPECT_NE(xml->find(testInterface), std::string::npos);
        EXPECT_NE(xml->find("Add"), std::string::npos);
    });
}
