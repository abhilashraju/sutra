#include "graphql_fixture.hpp"
#include "sutra/graphql/endpoint.hpp"
#include "sutra/http_client.hpp"
#include "sutra/http_server.hpp"

#include <gtest/gtest.h>

using namespace sutra;
using namespace sutra::graphql;
using nlohmann::json;

namespace
{

tcp::endpoint loopback()
{
    return tcp::endpoint(net::ip::make_address("127.0.0.1"), 0);
}

/** Starts a server with the fixture endpoint and runs `body(client)`. */
template <class Body>
void withClient(Body& body, EndpointOptions options = {})
{
    net::io_context ioc;
    HttpRouter router;
    registerGraphqlEndpoint(router, graphql_fixture::makeExecutor(), options);
    HttpServer server(ioc.get_executor(), loopback(), router);
    server.start();

    exec::async_scope scope;
    bool failed = false;
    auto wrapper = [&]() -> Task<void> {
        try
        {
            HttpClient client(ioc.get_executor());
            co_await client.connect(
                "127.0.0.1", std::to_string(server.localEndpoint().port()));
            co_await body(client);
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << "client failed: " << e.what();
            failed = true;
        }
        server.stop();
    };
    spawnTask(scope, wrapper(), "graphql endpoint test");
    ioc.run();
    EXPECT_FALSE(failed);
}

HttpRequest post(std::string body,
                 std::string_view contentType = "application/json")
{
    HttpRequest req{http::verb::post, "/graphql", 11};
    req.set(http::field::content_type, contentType);
    req.body() = std::move(body);
    return req;
}

} // namespace

TEST(GraphqlEndpoint, PostQuery)
{
    auto body = [](HttpClient& client) -> Task<void> {
        json payload{{"query", "{ sensors { Id Status { Health } } }"}};
        auto res = co_await client.send(post(payload.dump()));
        EXPECT_EQ(res.result(), http::status::ok);
        EXPECT_EQ(res[http::field::content_type], "application/json");
        json out = json::parse(res.body());
        EXPECT_EQ(out["data"]["sensors"].size(), 2u);
        EXPECT_EQ(out["data"]["sensors"][0]["Status"]["Health"], "OK");
        EXPECT_FALSE(out.contains("errors"));
    };
    withClient(body);
}

TEST(GraphqlEndpoint, PostWithVariables)
{
    auto body = [](HttpClient& client) -> Task<void> {
        json payload{{"query", "query Q($id: String) { sensor(id: $id) { "
                               "Reading } }"},
                     {"variables", {{"id", "fan_1"}}}};
        auto res = co_await client.send(post(payload.dump()));
        EXPECT_EQ(res.result(), http::status::ok);
        EXPECT_EQ(json::parse(res.body())["data"]["sensor"]["Reading"], 3200.0);
    };
    withClient(body);
}

TEST(GraphqlEndpoint, RawGraphqlContentType)
{
    auto body = [](HttpClient& client) -> Task<void> {
        auto res =
            co_await client.send(post("{ version }", "application/graphql"));
        EXPECT_EQ(res.result(), http::status::ok);
        EXPECT_EQ(json::parse(res.body())["data"]["version"], "1.2.3");
    };
    withClient(body);
}

TEST(GraphqlEndpoint, GetQuery)
{
    auto body = [](HttpClient& client) -> Task<void> {
        HttpRequest req{
            http::verb::get,
            "/graphql?query=query%20Q(%24i%3A%20String)%20%7B%20sensor(id%3A%20"
            "%24i)%20%7B%20Id%20%7D%20%7D&variables=%7B%22i%22%3A%22fan_1%22%7D",
            11};
        auto res = co_await client.send(std::move(req));
        EXPECT_EQ(res.result(), http::status::ok);
        EXPECT_EQ(json::parse(res.body())["data"]["sensor"]["Id"], "fan_1");
    };
    withClient(body);
}

TEST(GraphqlEndpoint, GetDisabled)
{
    auto body = [](HttpClient& client) -> Task<void> {
        HttpRequest req{http::verb::get, "/graphql?query=%7Bversion%7D", 11};
        auto res = co_await client.send(std::move(req));
        EXPECT_EQ(res.result(), http::status::method_not_allowed);
    };
    EndpointOptions options;
    options.allowGet = false;
    withClient(body, options);
}

TEST(GraphqlEndpoint, QueryErrorsStay200)
{
    auto body = [](HttpClient& client) -> Task<void> {
        json payload{{"query", "{ nope }"}};
        auto res = co_await client.send(post(payload.dump()));
        EXPECT_EQ(res.result(), http::status::ok);
        json out = json::parse(res.body());
        EXPECT_FALSE(out.contains("data"));
        EXPECT_NE(out["errors"][0]["message"].get<std::string>().find(
                      "Unknown field"),
                  std::string::npos);
    };
    withClient(body);
}

TEST(GraphqlEndpoint, BadRequestsAre400)
{
    auto body = [](HttpClient& client) -> Task<void> {
        for (const char* bad :
             {"not json", "[1,2]", "{}", R"({"query": 5})",
              R"({"query": "{ version }", "variables": [1]})"})
        {
            auto res = co_await client.send(post(bad));
            EXPECT_EQ(res.result(), http::status::bad_request) << bad;
            EXPECT_TRUE(json::parse(res.body()).contains("errors")) << bad;
        }
        auto res = co_await client.send(post(R"({"query": ""})"));
        EXPECT_EQ(res.result(), http::status::bad_request);
    };
    withClient(body);
}

TEST(GraphqlEndpoint, OversizedQueryIsRejected)
{
    auto body = [](HttpClient& client) -> Task<void> {
        json payload{{"query", "{ version " + std::string(300, ' ') + "}"}};
        auto res = co_await client.send(post(payload.dump()));
        EXPECT_EQ(res.result(), http::status::bad_request);
        EXPECT_NE(res.body().find("too large"), std::string::npos);

        json small{{"query", "{ version }"}};
        res = co_await client.send(post(small.dump()));
        EXPECT_EQ(res.result(), http::status::ok);
    };
    EndpointOptions options;
    options.maxQueryBytes = 100;
    withClient(body, options);
}
