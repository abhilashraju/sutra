/**
 * @file http_target.hpp
 * @brief Request context and request-target parsing.
 */
#pragma once

#include "sutra/config.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace sutra
{

/** @brief Per-request information handed to route handlers. */
struct RequestContext
{
    /** Decoded path without the query string. */
    std::string path;
    /** Decoded query parameters. */
    std::unordered_map<std::string, std::string> query;
    /** Values of `{name}` path segments of the matched route. */
    std::unordered_map<std::string, std::string> params;
    tcp::endpoint remote;

    std::string queryParam(const std::string& name,
                           const std::string& fallback = {}) const
    {
        auto it = query.find(name);
        return it == query.end() ? fallback : it->second;
    }

    std::string param(const std::string& name) const
    {
        auto it = params.find(name);
        return it == params.end() ? std::string() : it->second;
    }
};

/** @brief Decodes %XX escapes (and '+' when `plusAsSpace`). */
inline std::string percentDecode(std::string_view in, bool plusAsSpace)
{
    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        char c = in[i];
        if (c == '%' && i + 2 < in.size() + 0 && hexValue(in[i + 1]) >= 0 &&
            hexValue(in[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>(
                hexValue(in[i + 1]) * 16 + hexValue(in[i + 2])));
            i += 2;
        }
        else if (c == '+' && plusAsSpace)
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

/** @brief Splits `target` into decoded path and query parameters. */
inline void parseTarget(std::string_view target, RequestContext& ctx)
{
    auto fragment = target.find('#');
    if (fragment != std::string_view::npos)
    {
        target = target.substr(0, fragment);
    }
    auto questionMark = target.find('?');
    ctx.path = percentDecode(target.substr(0, questionMark), false);
    if (questionMark == std::string_view::npos)
    {
        return;
    }
    std::string_view rest = target.substr(questionMark + 1);
    while (!rest.empty())
    {
        auto amp = rest.find('&');
        std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view()
                                             : rest.substr(amp + 1);
        if (pair.empty())
        {
            continue;
        }
        auto eq = pair.find('=');
        std::string key = percentDecode(pair.substr(0, eq), true);
        std::string value = eq == std::string_view::npos
                                ? std::string()
                                : percentDecode(pair.substr(eq + 1), true);
        ctx.query[std::move(key)] = std::move(value);
    }
}

} // namespace sutra
