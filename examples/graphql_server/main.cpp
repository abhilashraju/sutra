/**
 * @file main.cpp
 * @brief GraphQL server over HTTP(S) with a Redfish-like schema.
 *
 * Root fields come from a Redfish service ("redfish", by default the one on
 * https://127.0.0.1:443). With --bus session|system, and when built with
 * D-Bus support, the live bus ("dbus") is added as an optional second source.
 *
 *   graphql_server [--address 127.0.0.1] [--port 8080] [--schema FILE]
 *                  [--redfish URL] [--redfish-user NAME] [--redfish-ca FILE]
 *                  [--bus session|system|none] [--tls]
 *                  [--cert FILE --key FILE]   (--help lists the short forms)
 *
 * Redfish secrets are read from the environment, never from the command
 * line: SUTRA_REDFISH_TOKEN (X-Auth-Token) or SUTRA_REDFISH_PASSWORD (with
 * --redfish-user, HTTP Basic).
 *
 * With --tls and no --cert/--key a throw-away self-signed certificate is
 * generated at start-up (the certificate is printed to stdout so clients can
 * pin it with `curl --cacert`). The server refuses wildcard addresses.
 */
#include "sutra/cert_generator.hpp"
#include "sutra/command_line.hpp"
#include "sutra/graphql/endpoint.hpp"
#include "sutra/graphql/redfish_source.hpp"
#include "sutra/http_server.hpp"

#ifdef SUTRA_HAVE_DBUS
#include "sutra/graphql/dbus_source.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <expected>
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
    std::string redfishUrl = "https://127.0.0.1:443";
    std::string bus = "none";
    bool tls = false;
    std::string certPath;
    std::string keyPath;
    std::string redfishUser;
    std::string redfishCa;
};

CommandLine makeCommandLine()
{
    CommandLine cmd;
    cmd.addOption("--address,-a", "ADDR",
                  "listen address (default 127.0.0.1, wildcards refused)");
    cmd.addOption("--port,-p", "N", "listen port (default 8080)");
    cmd.addOption("--schema,-s", "FILE",
                  "schema JSON (default: the example schema.json)");
    cmd.addOption("--redfish", "URL",
                  "Redfish service to read from (default "
                  "https://127.0.0.1:443)");
    cmd.addOption("--redfish-user", "NAME",
                  "Redfish user (password: SUTRA_REDFISH_PASSWORD)");
    cmd.addOption("--redfish-ca", "FILE",
                  "CA certificate (PEM) to trust for an https Redfish service");
    cmd.addOption("--bus,-b", "session|system|none",
                  "also read D-Bus fields from this bus (default none)");
    cmd.addFlag("--tls,-t", "serve HTTPS");
    cmd.addOption("--cert,-c", "FILE", "TLS certificate (PEM)");
    cmd.addOption("--key,-k", "FILE", "TLS private key (PEM)");
    cmd.addFlag("--help,-h", "show this help");
    return cmd;
}

/** @return the options, or an error message to print before the usage. */
std::expected<Options, std::string> readOptions(const CommandLine& cmd)
{
    Options options;
    options.address = cmd.valueOr("--address", options.address);
    options.schemaPath = cmd.valueOr("--schema", options.schemaPath);
    options.bus = cmd.valueOr("--bus", options.bus);
    options.tls = cmd.isSet("--tls");
    options.certPath = cmd.valueOr("--cert", "");
    options.keyPath = cmd.valueOr("--key", "");
    options.redfishUrl = cmd.valueOr("--redfish", options.redfishUrl);
    options.redfishUser = cmd.valueOr("--redfish-user", "");
    options.redfishCa = cmd.valueOr("--redfish-ca", "");

    auto port = cmd.number<uint16_t>("--port");
    if (!port)
    {
        return std::unexpected(port.error());
    }
    options.port = port->value_or(options.port);

    if (options.bus != "session" && options.bus != "system" &&
        options.bus != "none")
    {
        return std::unexpected("option '--bus' must be session, system or "
                               "none, got '" +
                               options.bus + "'");
    }
    if (options.certPath.empty() != options.keyPath.empty())
    {
        return std::unexpected("--cert and --key must be given together");
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

/** @brief Registers the Redfish source for `options.redfishUrl`. */
std::expected<void, std::string> addRedfishSource(
    net::io_context& ioc, const Options& options,
    graphql::SourceRegistry& registry)
{
    graphql::RedfishCredentials credentials;
    if (const char* token = std::getenv("SUTRA_REDFISH_TOKEN"))
    {
        credentials.token = token;
    }
    if (const char* password = std::getenv("SUTRA_REDFISH_PASSWORD"))
    {
        credentials.password = password;
    }
    credentials.username = options.redfishUser;

    try
    {
        std::shared_ptr<graphql::RedfishBackend> backend;
        if (parseUrl(options.redfishUrl).tls)
        {
            std::string trusted;
            if (!options.redfishCa.empty())
            {
                auto pem = readFile(options.redfishCa);
                if (!pem)
                {
                    return std::unexpected("cannot read --redfish-ca file");
                }
                trusted = std::move(*pem);
            }
            auto context =
                std::make_shared<ssl::context>(makeClientContext(trusted));
            auto pool = HttpsConnectionPool::create(ioc.get_executor(),
                                                    context.get(), {}, context);
            backend = std::make_shared<graphql::RedfishHttpsBackend>(
                pool, options.redfishUrl, credentials);
        }
        else
        {
            if (!credentials.token.empty() || !credentials.password.empty())
            {
                logWarning("sending Redfish credentials over plain http");
            }
            auto pool = HttpConnectionPool::create(ioc.get_executor());
            backend = std::make_shared<graphql::RedfishHttpBackend>(
                pool, options.redfishUrl, credentials);
        }
        registry.add(graphql::redfishSourceName,
                     std::make_shared<graphql::RedfishSource>(backend));
    }
    catch (const std::exception& e)
    {
        return std::unexpected(std::string("--redfish: ") + e.what());
    }
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    CommandLine cmd = makeCommandLine();
    if (auto parsed = cmd.parse(argc, argv); !parsed)
    {
        std::fprintf(stderr, "%s\n%s", parsed.error().c_str(),
                     cmd.usage(argv[0]).c_str());
        return 2;
    }
    if (cmd.isSet("--help"))
    {
        std::fputs(cmd.usage(argv[0]).c_str(), stdout);
        return 0;
    }
    auto parsedOptions = readOptions(cmd);
    if (!parsedOptions)
    {
        std::fprintf(stderr, "%s\n%s", parsedOptions.error().c_str(),
                     cmd.usage(argv[0]).c_str());
        return 2;
    }
    const Options options = *parsedOptions;

    auto schema = graphql::Schema::fromFile(options.schemaPath);
    if (!schema)
    {
        logError("{}", schema.error());
        return 1;
    }

    net::io_context ioc;
    auto registry = std::make_shared<graphql::SourceRegistry>();
    if (auto added = addRedfishSource(ioc, options, *registry); !added)
    {
        logError("{}", added.error());
        return 1;
    }

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

    // Fields whose source is not registered (no D-Bus support or no --bus) are
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
