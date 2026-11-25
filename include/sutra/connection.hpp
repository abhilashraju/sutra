/**
 * @file connection.hpp
 * @brief Buffered, task based I/O over any Asio stream.
 *
 * `Connection<Stream>` owns a stream plus a read-ahead buffer and exposes
 * `write`, `readSome`, `readUntil` and `readExactly` as Tasks. It works with
 * `tcp::socket` and `ssl::stream<tcp::socket>` alike. The read-ahead buffer is
 * a `beast::flat_buffer`, so Beast's HTTP algorithms can use it directly
 * through `buffer()` and `run()`.
 *
 * Timeouts: an optional per-operation timeout is enforced with a watchdog
 * timer that cancels the socket. Beast's `tcp_stream` timer is deliberately
 * not used: its timer handler outlives the operation and touches the
 * use_sender operation state after it was destroyed.
 *
 * Outcomes of an operation: success returns normally, a timeout throws
 * `system_error(timed_out)`, any other failure throws `system_error`, and a
 * stop request (cancellation) completes the task as stopped.
 */
#pragma once

#include "sutra/config.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace sutra
{

template <class Stream>
class Connection
{
  public:
    using stream_type = Stream;
    using Duration = std::chrono::steady_clock::duration;

    /** Upper bound for data buffered ahead of the reader. */
    static constexpr size_t defaultMaxBuffer = 16 * 1024 * 1024;

    explicit Connection(Stream&& newStream) :
        stream_(std::move(newStream)),
        watchdog(stream_.lowest_layer().get_executor()), inbuf(defaultMaxBuffer)
    {}

    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Stream& stream()
    {
        return stream_;
    }

    auto& lowest()
    {
        return stream_.lowest_layer();
    }

    /** @brief Read-ahead buffer, for use with Beast algorithms. */
    beast::flat_buffer& buffer()
    {
        return inbuf;
    }

    /** @brief Applies to every following read/write; nullopt disables it. */
    void setTimeout(std::optional<Duration> value)
    {
        timeout = value;
    }

    std::optional<Duration> getTimeout() const
    {
        return timeout;
    }

    /** @brief Writes all of `data`. */
    Task<size_t> write(std::string_view data)
    {
        arm(timeout);
        auto [ec, count] = co_await net::async_write(
            stream_, net::buffer(data.data(), data.size()), useSenderNoThrow);
        co_await finishOp(ec);
        co_return count;
    }

    /**
     * @brief Like write(), but reports failure as a value instead of throwing.
     * @return the failure (a timeout is `timed_out`), or a default error_code
     *         on success. Cancellation still completes the task as stopped.
     */
    Task<error_code> tryWrite(std::string_view data)
    {
        arm(timeout);
        auto [ec, count] = co_await net::async_write(
            stream_, net::buffer(data.data(), data.size()), useSenderNoThrow);
        static_cast<void>(count);
        co_return co_await checkOp(ec);
    }

    /**
     * @brief Reads up to `maxBytes`. Returns buffered data first.
     * @return the bytes read, or an empty string at end of stream.
     */
    Task<std::string> readSome(size_t maxBytes = 4096)
    {
        if (inbuf.size() != 0)
        {
            co_return take(std::min(maxBytes, inbuf.size()));
        }
        arm(timeout);
        std::string chunk(maxBytes, '\0');
        auto [ec, count] = co_await stream_.async_read_some(net::buffer(chunk),
                                                            useSenderNoThrow);
        if (isEndOfStream(ec))
        {
            disarm();
            co_return std::string();
        }
        co_await finishOp(ec);
        chunk.resize(count);
        co_return chunk;
    }

    /**
     * @brief Reads until `delimiter` is seen.
     * @return everything up to and including the delimiter. Extra bytes
     *         stay buffered for the next read.
     */
    Task<std::string> readUntil(std::string delimiter)
    {
        size_t scanned = 0;
        for (;;)
        {
            std::string_view view(static_cast<const char*>(inbuf.data().data()),
                                  inbuf.size());
            auto at = view.find(delimiter, scanned);
            if (at != std::string_view::npos)
            {
                co_return take(at + delimiter.size());
            }
            scanned = view.size() >= delimiter.size()
                          ? view.size() - delimiter.size() + 1
                          : 0;
            co_await fill();
        }
    }

    /** @brief Reads exactly `count` bytes (EOF before that is an error). */
    Task<std::string> readExactly(size_t count)
    {
        while (inbuf.size() < count)
        {
            co_await fill();
        }
        co_return take(count);
    }

    /**
     * @brief Runs one Asio operation under the per-operation timeout.
     *
     * `initiate` receives the completion token and must start an operation
     * that completes with `(error_code, size_t)`, for example
     * `run([&](auto token) { return http::async_read(s, b, m, token); })`.
     * @return the operation's byte count.
     */
    template <class Initiate>
    Task<size_t> run(Initiate initiate)
    {
        arm(timeout);
        auto result = co_await initiate(useSenderNoThrow);
        co_await finishOp(std::get<0>(result));
        co_return std::get<1>(result);
    }

    /** @brief Like run(), but reports failure as a value (see tryWrite). */
    template <class Initiate>
    Task<error_code> tryRun(Initiate initiate)
    {
        arm(timeout);
        auto result = co_await initiate(useSenderNoThrow);
        co_return co_await checkOp(std::get<0>(result));
    }

    /** @brief TLS handshake (TLS streams only), bounded by `limit`. */
    Task<void> handshake(ssl::stream_base::handshake_type type,
                         std::optional<Duration> limit)
    {
        static_assert(!std::is_same_v<Stream, tcp::socket>,
                      "handshake needs a TLS stream");
        arm(limit);
        auto result = co_await stream_.async_handshake(type, useSenderNoThrow);
        co_await finishOp(std::get<0>(result));
    }

    /** @brief Like handshake(), but reports failure as a value. */
    Task<error_code> tryHandshake(ssl::stream_base::handshake_type type,
                                  std::optional<Duration> limit)
    {
        static_assert(!std::is_same_v<Stream, tcp::socket>,
                      "handshake needs a TLS stream");
        arm(limit);
        auto result = co_await stream_.async_handshake(type, useSenderNoThrow);
        co_return co_await checkOp(std::get<0>(result));
    }

    /** @brief Shuts the socket down and closes it. Never throws. */
    void close() noexcept
    {
        disarm();
        error_code ec;
        lowest().shutdown(tcp::socket::shutdown_both, ec);
        lowest().close(ec);
    }

    /**
     * @brief Performs the TLS close_notify exchange (TLS streams only, bounded
     *        by `limit`) and then closes. Never throws.
     */
    Task<void> gracefulClose(Duration limit = 5s)
    {
        if constexpr (!std::is_same_v<Stream, tcp::socket>)
        {
            arm(limit);
            auto result = co_await stream_.async_shutdown(useSenderNoThrow);
            static_cast<void>(result);
            disarm();
        }
        close();
    }

    bool isOpen()
    {
        return lowest().is_open();
    }

    /**
     * @brief Starts the watchdog for one operation; nullopt means none.
     * Pair every arm() with `co_await finishOp(ec)` on the operation result.
     */
    void arm(std::optional<Duration> limit)
    {
        timedOut = false;
        if (!limit)
        {
            return;
        }
        watchdog.expires_after(*limit);
        watchdog.async_wait([this](const error_code& ec) {
            if (!ec)
            {
                timedOut = true;
                error_code ignored;
                lowest().cancel(ignored);
            }
        });
    }

    /**
     * @brief Non-throwing form of finishOp().
     * @return success, `timed_out` for a watchdog expiry, or the failure.
     *         A cancellation that is not a timeout completes the task as
     *         stopped.
     */
    Task<error_code> checkOp(error_code ec)
    {
        disarm();
        if (!ec)
        {
            co_return error_code();
        }
        if (ec == net::error::operation_aborted)
        {
            if (timedOut)
            {
                co_return make_error_code(boost::system::errc::timed_out);
            }
            co_await ex::just_stopped();
        }
        co_return ec;
    }

    /** @brief Turns the result of an armed operation into the Task outcome. */
    Task<void> finishOp(error_code ec)
    {
        if (auto failure = co_await checkOp(ec))
        {
            throw system_error(failure);
        }
    }

  private:
    /** @brief Reads at least one more byte into the buffer; EOF throws. */
    Task<void> fill()
    {
        arm(timeout);
        auto [ec, got] = co_await stream_.async_read_some(inbuf.prepare(4096),
                                                          useSenderNoThrow);
        if (!ec)
        {
            inbuf.commit(got);
        }
        co_await finishOp(ec);
    }

    static bool isEndOfStream(const error_code& ec)
    {
        return ec == net::error::eof || ec == ssl::error::stream_truncated;
    }

    void disarm()
    {
        watchdog.cancel();
    }

    std::string take(size_t count)
    {
        std::string out(static_cast<const char*>(inbuf.data().data()), count);
        inbuf.consume(count);
        return out;
    }

    Stream stream_;
    net::steady_timer watchdog;
    beast::flat_buffer inbuf;
    std::optional<Duration> timeout;
    bool timedOut = false;
};

/** @brief Plain TCP connection. */
using TcpConnection = Connection<tcp::socket>;

/** @brief TLS connection over TCP. */
using TlsConnection = Connection<ssl::stream<tcp::socket>>;

} // namespace sutra
