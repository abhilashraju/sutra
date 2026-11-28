#include "sutra/cert_generator.hpp"
#include "sutra/http_client.hpp"
#include "sutra/http_server.hpp"

#include <gtest/gtest.h>

using namespace sutra;

namespace
{

HttpRouter makeRouter()
{
    HttpRouter router;
    router.get("/hello", [](HttpRequest&, RequestContext&) {
        return makeResponse(http::status::ok, "world");
    });
    router.get("/items/{id}", [](HttpRequest&, RequestContext& ctx) {
        return makeResponse(http::status::ok,
                            ctx.param("id") + ":" + ctx.queryParam("q"));
    });
    router.post("/echo",
                [](HttpRequest& req, RequestContext&) -> Task<HttpResponse> {
                    co_return jsonResponse(http::status::ok, req.body());
                });
    router.get("/boom", [](HttpRequest&, RequestContext&) -> HttpResponse {
        throw std::runtime_error("handler exploded");
    });
    router.sse("/events",
               [](HttpRequest&, RequestContext&, SseWriter& out) -> Task<void> {
                   for (int i = 0; i < 3; ++i)
                   {
                       if (!co_await out.write("tick " + std::to_string(i)))
                       {
                           co_return;
                       }
                   }
               });
    return router;
}

HttpRequest get(const std::string& target)
{
    HttpRequest req{http::verb::get, target, 11};
    return req;
}

HttpRequest post(const std::string& target, std::string body)
{
    HttpRequest req{http::verb::post, target, 11};
    req.body() = std::move(body);
    return req;
}

/** Runs `body` as a task, then stops `server` so ioc.run() returns. */
template <class Server, class Body>
void runClient(net::io_context& ioc, Server& server, Body body)
{
    exec::async_scope scope;
    bool failed = false;
    auto wrapper = [&]() -> Task<void> {
        try
        {
            co_await body();
        }
        catch (const std::exception& e)
        {
            ADD_FAILURE() << "client failed: " << e.what();
            failed = true;
        }
        server.stop();
    };
    spawnTask(scope, wrapper(), "http client");
    ioc.run();
    EXPECT_FALSE(failed);
}

} // namespace

TEST(Http, RoutesKeepAliveAndErrors)
{
    net::io_context ioc;
    auto router = makeRouter();
    HttpServer server(ioc.get_executor(),
                      tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                      router);
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    runClient(ioc, server, [&]() -> Task<void> {
        HttpClient client(ioc.get_executor());
        client.setTimeout(5s);
        EXPECT_TRUE(co_await client.connect("127.0.0.1", port));

        // Several requests over one connection.
        auto r1 = co_await client.send(get("/hello"));
        EXPECT_TRUE(r1);
        if (!r1)
        {
            co_return;
        }
        EXPECT_EQ(r1->result(), http::status::ok);
        EXPECT_EQ(r1->body(), "world");

        auto r2 = co_await client.send(get("/items/42?q=a%20b&x=1"));
        EXPECT_TRUE(r2);
        EXPECT_EQ(r2 ? r2->body() : "", "42:a b");

        auto r3 = co_await client.send(post("/echo", R"({"a":1})"));
        EXPECT_TRUE(r3);
        if (r3)
        {
            EXPECT_EQ(r3->body(), R"({"a":1})");
            EXPECT_EQ((*r3)[http::field::content_type], "application/json");
        }

        auto r4 = co_await client.send(get("/missing"));
        EXPECT_TRUE(r4);
        EXPECT_EQ(r4 ? r4->result() : http::status::unknown,
                  http::status::not_found);

        auto r5 = co_await client.send(post("/hello", "x"));
        EXPECT_TRUE(r5);
        EXPECT_EQ(r5 ? r5->result() : http::status::unknown,
                  http::status::method_not_allowed);

        auto r6 = co_await client.send(get("/boom"));
        EXPECT_TRUE(r6);
        if (r6)
        {
            EXPECT_EQ(r6->result(), http::status::internal_server_error);
            EXPECT_EQ(r6->body().find("exploded"), std::string::npos);
        }
    });
}

TEST(Http, RejectsOversizedBody)
{
    net::io_context ioc;
    auto router = makeRouter();
    HttpServerOptions options;
    options.maxBodyBytes = 16;
    HttpServer server(ioc.get_executor(),
                      tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                      router, options);
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    runClient(ioc, server, [&]() -> Task<void> {
        HttpClient client(ioc.get_executor());
        client.setTimeout(5s);
        EXPECT_TRUE(co_await client.connect("127.0.0.1", port));
        auto res = co_await client.send(post("/echo", std::string(1024, 'x')));
        EXPECT_TRUE(res);
        EXPECT_EQ(res ? res->result() : http::status::unknown,
                  http::status::payload_too_large);
    });
}

TEST(Http, ServerSentEvents)
{
    net::io_context ioc;
    auto router = makeRouter();
    HttpServer server(ioc.get_executor(),
                      tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                      router);
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    std::vector<std::string> events;
    runClient(ioc, server, [&]() -> Task<void> {
        HttpClient client(ioc.get_executor());
        client.setTimeout(5s);
        EXPECT_TRUE(co_await client.connect("127.0.0.1", port));
        auto status =
            co_await client.subscribe(get("/events"), [&](std::string data) {
                events.push_back(data);
                return true;
            });
        EXPECT_TRUE(status);
        EXPECT_EQ(status.value_or(http::status::unknown), http::status::ok);
    });
    EXPECT_EQ(events, (std::vector<std::string>{"tick 0", "tick 1", "tick 2"}));
}

TEST(Https, VerifiedRoundTrip)
{
    net::io_context ioc;
    auto pem = generateSelfSignedCertificate("localhost");
    ASSERT_TRUE(pem);
    auto serverContext = makeServerContext(pem->certificate, pem->privateKey);
    auto clientContext = makeClientContext(pem->certificate);
    ASSERT_TRUE(serverContext);
    ASSERT_TRUE(clientContext);

    auto router = makeRouter();
    HttpServer server(ioc.get_executor(),
                      tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                      router, *serverContext);
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    runClient(ioc, server, [&]() -> Task<void> {
        HttpsClient client(ioc.get_executor(), &*clientContext);
        client.setTimeout(5s);
        EXPECT_TRUE(co_await client.connect("localhost", port));
        auto res = co_await client.send(get("/hello"));
        EXPECT_TRUE(res);
        EXPECT_EQ(res ? res->body() : "", "world");
        auto res2 = co_await client.send(get("/items/7"));
        EXPECT_TRUE(res2);
        EXPECT_EQ(res2 ? res2->body() : "", "7:");
    });
}

TEST(Https, UntrustedCertificateIsRejected)
{
    net::io_context ioc;
    auto pem = generateSelfSignedCertificate("localhost");
    auto other = generateSelfSignedCertificate("localhost");
    ASSERT_TRUE(pem);
    ASSERT_TRUE(other);
    auto serverContext = makeServerContext(pem->certificate, pem->privateKey);
    auto clientContext = makeClientContext(other->certificate);
    ASSERT_TRUE(serverContext);
    ASSERT_TRUE(clientContext);

    auto router = makeRouter();
    HttpServer server(ioc.get_executor(),
                      tcp::endpoint(net::ip::make_address("127.0.0.1"), 0),
                      router, *serverContext);
    ASSERT_TRUE(server.start());
    auto port = std::to_string(server.localEndpoint().port());

    bool rejected = false;
    exec::async_scope scope;
    auto task = [&]() -> Task<void> {
        HttpsClient client(ioc.get_executor(), &*clientContext);
        client.setTimeout(5s);
        auto connected = co_await client.connect("localhost", port);
        rejected = !connected && connected.error().category() ==
                                     net::error::get_ssl_category();
        server.stop();
    };
    spawnTask(scope, task(), "untrusted client");
    ioc.run();
    EXPECT_TRUE(rejected);
}
