/**
 * @file connection_pool.hpp
 * @brief Keep-alive connection pool for HttpClientT connections.
 *
 * `ConnectionPoolT<Stream>` hands out RAII leases (`PooledConnection`). A
 * lease returns its connection to the pool when it is destroyed or released,
 * unless it was marked bad. The pool enforces a per-host connection limit
 * (callers beyond it wait on a WaitQueue, bounded by `acquireTimeout`), drops
 * idle connections that exceeded `idleTimeout` and drops idle connections the
 * peer has closed in the meantime.
 *
 * Like every sutra component it is single threaded: use it from the
 * io_context thread only. Create it with `create()`; leases keep the pool
 * alive.
 */
#pragma once

#include "sutra/config.hpp"
#include "sutra/http_client.hpp"
#include "sutra/log.hpp"
#include "sutra/wait_queue.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sutra
{

struct ConnectionPoolConfig
{
    /** Leased plus connecting connections allowed per endpoint. */
    size_t maxConnectionsPerHost = 5;
    /** Idle connections older than this are closed instead of reused. */
    std::chrono::steady_clock::duration idleTimeout = 30s;
    /** How long acquire() waits for a free slot before timing out. */
    std::chrono::steady_clock::duration acquireTimeout = 10s;
    /** Limit for resolve + connect (+ TLS handshake) of a new connection. */
    std::chrono::steady_clock::duration connectTimeout = 10s;
    /**
     * TLS pools only: when not empty, SNI and certificate-name verification
     * use this name instead of the host being connected to. Verification stays
     * on; this is for servers whose certificate names differ from the address
     * used to reach them (for example `127.0.0.1` vs `CN=bmc.peer`).
     */
    std::string tlsServerName;
    /** Per-operation limit applied to connections handed out. */
    std::chrono::steady_clock::duration requestTimeout = 60s;
};

/** @brief Identifies a pool bucket. */
struct EndpointKey
{
    std::string host;
    std::string port;
    bool tls = false;

    friend auto operator<=>(const EndpointKey&, const EndpointKey&) = default;
};

template <class Stream>
class ConnectionPoolT;

/**
 * @brief RAII lease of a pooled connection.
 *
 * Destroying (or `release()`ing) the lease gives the connection back to the
 * pool, or closes it when `markBad()` was called. A default constructed lease
 * is empty.
 */
template <class Stream>
class PooledConnection
{
  public:
    using Client = HttpClientT<Stream>;

    PooledConnection() = default;

    PooledConnection(std::shared_ptr<ConnectionPoolT<Stream>> owner,
                     EndpointKey endpointKey, std::unique_ptr<Client> conn,
                     bool wasReused) :
        pool(std::move(owner)), key(std::move(endpointKey)),
        client(std::move(conn)), reusedConnection(wasReused)
    {}

    ~PooledConnection()
    {
        release();
    }

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;
    PooledConnection(PooledConnection&&) noexcept = default;

    PooledConnection& operator=(PooledConnection&& other) noexcept
    {
        if (this != &other)
        {
            release();
            pool = std::move(other.pool);
            key = std::move(other.key);
            client = std::move(other.client);
            bad = other.bad;
            reusedConnection = other.reusedConnection;
        }
        return *this;
    }

    Client& get()
    {
        return *client;
    }

    Client& operator*()
    {
        return *client;
    }

    Client* operator->()
    {
        return client.get();
    }

    explicit operator bool() const
    {
        return client != nullptr;
    }

    /** @brief True when the connection came from the idle list. */
    bool reused() const
    {
        return reusedConnection;
    }

    /** @brief The connection will be closed instead of returned. */
    void markBad()
    {
        bad = true;
    }

    /** @brief Undoes markBad(): the connection goes back to the pool. */
    void markReusable()
    {
        bad = false;
    }

    /** @brief Returns the connection to the pool now. */
    void release();

  private:
    std::shared_ptr<ConnectionPoolT<Stream>> pool;
    EndpointKey key;
    std::unique_ptr<Client> client;
    bool bad = false;
    bool reusedConnection = false;
};

template <class Stream>
class ConnectionPoolT :
    public std::enable_shared_from_this<ConnectionPoolT<Stream>>
{
  public:
    using Client = HttpClientT<Stream>;
    using Lease = PooledConnection<Stream>;

    static constexpr bool isTls = Client::isTls;

    /**
     * @param executor      io_context executor.
     * @param clientContext TLS context (required for TLS pools); it must
     *                      outlive the pool unless `ownedContext` is given.
     * @param ownedContext  optional owner of `clientContext`.
     *
     * Always construct through create(): leases need shared ownership.
     */
    static std::shared_ptr<ConnectionPoolT> create(
        net::any_io_executor executor, ssl::context* clientContext = nullptr,
        ConnectionPoolConfig poolConfig = {},
        std::shared_ptr<ssl::context> ownedContext = nullptr)
    {
        return std::shared_ptr<ConnectionPoolT>(
            new ConnectionPoolT(std::move(executor), clientContext, poolConfig,
                                std::move(ownedContext)));
    }

    ConnectionPoolT(const ConnectionPoolT&) = delete;
    ConnectionPoolT& operator=(const ConnectionPoolT&) = delete;

    const ConnectionPoolConfig& getConfig() const
    {
        return config;
    }
    const net::any_io_executor& getExecutor() const
    {
        return executor;
    }

    /**
     * @brief Leases a connection to host:port.
     *
     * Reuses a live idle connection when there is one, opens a new one while
     * below the per-host limit, and otherwise waits for a lease to be
     * returned.
     * @return the lease, the connect/handshake failure, or `timed_out` when
     *         no slot became free within `acquireTimeout`.
     */
    Task<Expected<Lease>> acquire(std::string host, std::string port)
    {
        auto self = this->shared_from_this();
        EndpointKey key{std::move(host), std::move(port), isTls};
        HostState& state = hostState(key);
        const auto deadline =
            std::chrono::steady_clock::now() + config.acquireTimeout;

        for (;;)
        {
            if (auto client = takeIdle(state))
            {
                ++state.active;
                co_return Lease(self, key, std::move(client), true);
            }
            if (state.active < config.maxConnectionsPerHost)
            {
                co_return co_await open(self, key, state);
            }

            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero())
            {
                co_return std::unexpected(
                    make_error_code(boost::system::errc::timed_out));
            }
            const bool woken = co_await state.waiters.wait(remaining);
            auto token = co_await ex::read_env(ex::get_stop_token);
            if (token.stop_requested())
            {
                co_await ex::just_stopped();
            }
            if (!woken)
            {
                co_return std::unexpected(
                    make_error_code(boost::system::errc::timed_out));
            }
        }
    }

    /** @brief Called by PooledConnection; not meant for direct use. */
    void giveBack(const EndpointKey& key, std::unique_ptr<Client> client,
                  bool reusable)
    {
        HostState& state = hostState(key);
        if (state.active > 0)
        {
            --state.active;
        }
        if (client)
        {
            if (reusable && client->isOpen() && client->buffer().size() == 0)
            {
                state.idle.push_back(
                    {std::move(client), std::chrono::steady_clock::now()});
            }
            else
            {
                client->close();
            }
        }
        state.waiters.notifyOne();
    }

    /** @brief Closes every idle connection and wakes all waiters. */
    void clear()
    {
        for (auto& [key, state] : hosts)
        {
            state.idle.clear();
            state.waiters.notifyAll();
        }
    }

    size_t activeCount(const EndpointKey& key) const
    {
        auto it = hosts.find(key);
        return it == hosts.end() ? 0 : it->second.active;
    }

    size_t idleCount(const EndpointKey& key) const
    {
        auto it = hosts.find(key);
        return it == hosts.end() ? 0 : it->second.idle.size();
    }

    size_t waiterCount(const EndpointKey& key) const
    {
        auto it = hosts.find(key);
        return it == hosts.end() ? 0 : it->second.waiters.waiting();
    }

  private:
    struct IdleEntry
    {
        std::unique_ptr<Client> client;
        std::chrono::steady_clock::time_point since;
    };

    struct HostState
    {
        explicit HostState(net::any_io_executor executor) :
            waiters(std::move(executor))
        {}

        size_t active = 0;
        std::vector<IdleEntry> idle;
        WaitQueue waiters;
    };

    /** Frees a reserved slot unless dismissed (connect failed or stopped). */
    struct Reservation
    {
        ConnectionPoolT& pool;
        HostState& state;
        bool armed = true;

        ~Reservation()
        {
            if (armed)
            {
                if (state.active > 0)
                {
                    --state.active;
                }
                state.waiters.notifyOne();
            }
        }
    };

    ConnectionPoolT(net::any_io_executor ioExecutor, ssl::context* context,
                    ConnectionPoolConfig poolConfig,
                    std::shared_ptr<ssl::context> owned) :
        executor(std::move(ioExecutor)), tlsContext(context),
        ownedContext(std::move(owned)), config(poolConfig)
    {
        if constexpr (isTls)
        {
            if (tlsContext == nullptr)
            {
                throw std::invalid_argument("TLS pool needs an ssl::context");
            }
        }
    }

    HostState& hostState(const EndpointKey& key)
    {
        return hosts.try_emplace(key, executor).first->second;
    }

    Task<Expected<Lease>> open(std::shared_ptr<ConnectionPoolT> self,
                               EndpointKey key, HostState& state)
    {
        ++state.active;
        Reservation reservation{*this, state};
        auto client = std::make_unique<Client>(executor, tlsContext);
        client->setConnectTimeout(config.connectTimeout);
        auto connected =
            co_await client->connect(key.host, key.port, config.tlsServerName);
        if (!connected)
        {
            co_return std::unexpected(connected.error());
        }
        client->setTimeout(config.requestTimeout);
        reservation.armed = false;
        co_return Lease(std::move(self), std::move(key), std::move(client),
                        false);
    }

    /** @brief Newest idle connection that is still alive, or null. */
    std::unique_ptr<Client> takeIdle(HostState& state)
    {
        const auto now = std::chrono::steady_clock::now();
        while (!state.idle.empty())
        {
            IdleEntry entry = std::move(state.idle.back());
            state.idle.pop_back();
            if (now - entry.since > config.idleTimeout)
            {
                // Older entries are older still: drop them all.
                state.idle.clear();
                entry.client->close();
                return nullptr;
            }
            if (peerClosed(*entry.client))
            {
                logDebug("dropping idle connection closed by peer");
                entry.client->close();
                continue;
            }
            return std::move(entry.client);
        }
        return nullptr;
    }

    /**
     * @brief Non-blocking peek: true when the peer closed the connection or
     *        sent unexpected data while it sat idle.
     */
    static bool peerClosed(Client& client)
    {
        if (!client.isOpen())
        {
            return true;
        }
        char byte = 0;
        const auto got = ::recv(client.lowest().native_handle(), &byte, 1,
                                MSG_PEEK | MSG_DONTWAIT);
        if (got < 0)
        {
            // Nothing pending is the healthy case.
            return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
        }
        // 0: orderly shutdown by the peer; >0: unsolicited data.
        return true;
    }

    net::any_io_executor executor;
    ssl::context* tlsContext;
    std::shared_ptr<ssl::context> ownedContext;
    ConnectionPoolConfig config;
    std::map<EndpointKey, HostState> hosts;
};

template <class Stream>
void PooledConnection<Stream>::release()
{
    if (pool && client)
    {
        pool->giveBack(key, std::move(client), !bad);
    }
    pool.reset();
    client.reset();
}

using HttpConnectionPool = ConnectionPoolT<tcp::socket>;
using HttpsConnectionPool = ConnectionPoolT<ssl::stream<tcp::socket>>;

} // namespace sutra
