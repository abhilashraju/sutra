/**
 * @file main.cpp
 * @brief GraphQL server over HTTP(S) with a Redfish-like schema.
 *
 * Root fields come from the in-memory source ("memory") and, when built with
 * D-Bus support and a bus is reachable, from the live bus ("dbus").
 *
 *   graphql_server [--address 127.0.0.1] [--port 8080] [--schema FILE]
 *                  [--bus session|system|none] [--tls]
 *                  [--cert FILE --key FILE]
 *
 * With --tls and no --cert/--key a throw-away self-signed certificate is
 * generated at start-up (the certificate is printed to stdout so clients can
 * pin it with `curl --cacert`). The server refuses wildcard addresses.
 */
#include "sample_data.hpp"
#include "sutra/cert_generator.hpp"
#include "sutra/graphql/endpoint.hpp"
#include "sutra/http_server.hpp"

#ifdef SUTRA_HAVE_DBUS
#include "sutra/graphql/dbus_source.hpp"
#endif

#include <charconv>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#ifndef SUTRA_EXAMPLE_SCHEMA
#define SUTRA_EXAMPLE_SCHEMA "schema.json"
#endif

using namespace sutra;

namespace
{

struct Options
{
    std::string address = "127.0.0.1";
    uint16_t port = 8080;
    std::string schemaPath = SUTRA_EXAMPLE_SCHEMA;
    std::string bus = "system";
    bool tls = false;
    std::string certPath;
    std::string keyPath;
};

void usage()
{
    std::fputs(
        "usage: graphql_server [--address ADDR] [--port N] [--schema FILE]\n"
        "                      [--bus session|system|none] [--tls]\n"
        "                      [--cert FILE --key FILE]\n",
        stderr);
}

std::optional<Options> parseArgs(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--tls")
        {
            options.tls = true;
            continue;
        }
        if (i + 1 >= argc)
        {
            return std::nullopt;
        }
        std::string value = argv[++i];
        if (arg == "--address")
        {
            options.address = value;
        }
        else if (arg == "--port")
        {
            unsigned parsed = 0;
            auto [end, ec] = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc{} || end != value.data() + value.size() ||
                parsed > 65535)
            {
                return std::nullopt;
            }
            options.port = static_cast<uint16_t>(parsed);
        }
        else if (arg == "--schema")
        {
            options.schemaPath = value;
        }
        else if (arg == "--bus")
        {
            if (value != "session" && value != "system" && value != "none")
            {
                return std::nullopt;
            }
            options.bus = value;
        }
        else if (arg == "--cert")
        {
            options.certPath = value;
        }
        else if (arg == "--key")
        {
            options.keyPath = value;
        }
        else
        {
            return std::nullopt;
        }
    }
    if (options.certPath.empty() != options.keyPath.empty())
    {
        return std::nullopt;
    }
    return options;
}

std::optional<std::string> readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

int main(int argc, char** argv)
{
    auto parsed = parseArgs(argc, argv);
    if (!parsed)
    {
        usage();
        return 2;
    }
    const Options options = *parsed;

    auto schema = graphql::Schema::fromFile(options.schemaPath);
    if (!schema)
    {
        logError("{}", schema.error());
        return 1;
    }

    net::io_context ioc;
    auto registry = std::make_shared<graphql::SourceRegistry>();
    registry->add(graphql::defaultSourceName, example::makeSampleSource());

#ifdef SUTRA_HAVE_DBUS
    std::shared_ptr<sdbusplus::asio::connection> bus;
    if (options.bus != "none")
    {
        try
        {
            bus = std::make_shared<sdbusplus::asio::connection>(
                ioc, options.bus == "session" ? sdbusplus::bus::new_user()
                                              : sdbusplus::bus::new_system());
            registry->add(
                graphql::dbusSourceName,
                std::make_shared<graphql::DbusSource>(
                    std::make_shared<graphql::SutraDbusBackend>(bus)));
        }
        catch (const std::exception& e)
        {
            logWarning("D-Bus unavailable ({}); dbus fields are disabled",
                       e.what());
        }
    }
#endif

    // Fields whose source is not registered (no D-Bus support or no bus) are
    // dropped; a misconfigured source stays a hard error.
    std::vector<std::string> warnings;
    if (auto applied =
            graphql::applySourceRegistry(*schema, *registry, false, warnings);
        !applied)
    {
        logError("schema: {}", applied.error());
        return 1;
    }
    for (const auto& warning : warnings)
    {
        logWarning("{}", warning);
    }

    auto executor =
        std::make_shared<graphql::Executor>(std::move(*schema), registry);
    HttpRouter router;
    graphql::registerGraphqlEndpoint(router, executor);
    router.get("/health", [](HttpRequest&, RequestContext&) {
        return jsonResponse(http::status::ok, R"({"status":"ok"})");
    });

    std::optional<ssl::context> tlsContext;
    if (options.tls)
    {
        std::string certPem;
        std::string keyPem;
        if (options.certPath.empty())
        {
            PemPair pair = generateSelfSignedCertificate(
                "sutra-graphql", "DNS:localhost,IP:127.0.0.1", 1);
            certPem = pair.certificate;
            keyPem = pair.privateKey;
            std::printf("%s", certPem.c_str());
            std::fflush(stdout);
        }
        else
        {
            auto cert = readFile(options.certPath);
            auto key = readFile(options.keyPath);
            if (!cert || !key)
            {
                logError("cannot read the certificate or key file");
                return 1;
            }
            certPem = std::move(*cert);
            keyPem = std::move(*key);
        }
        tlsContext.emplace(makeServerContext(certPem, keyPem));
    }

    try
    {
        tcp::endpoint endpoint(net::ip::make_address(options.address),
                               options.port);
        std::optional<HttpServer> server;
        if (tlsContext)
        {
            server.emplace(ioc.get_executor(), endpoint, router, *tlsContext);
        }
        else
        {
            server.emplace(ioc.get_executor(), endpoint, router);
        }
        server->start();
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&server](const error_code&, int) {
            server->stop();
        });
        logInfo("listening on {}://{}:{}/graphql",
                options.tls ? "https" : "http", options.address,
                server->localEndpoint().port());
        ioc.run();
    }
    catch (const std::exception& e)
    {
        logError("server failed: {}", e.what());
        return 1;
    }
    return 0;
}
