/**
 * @file command_line.hpp
 * @brief Small declarative command line parser for sutra programs.
 *
 * Keeps coroserver's alias syntax (`"--port,-p"`: the first name is the
 * canonical one) and adds what coroserver's `parseCommandline()` lacks:
 * boolean flags, `--name=value`, errors for unknown options and missing
 * values, and checked number parsing.
 *
 * @code
 * CommandLine cmd;
 * cmd.addOption("--port,-p", "N", "port to listen on");
 * cmd.addFlag("--tls", "serve HTTPS");
 * if (auto parsed = cmd.parse(argc, argv); !parsed)
 * {
 *     std::fprintf(stderr, "%s\n%s", parsed.error().c_str(),
 *                  cmd.usage("prog").c_str());
 *     return 2;
 * }
 * auto port = cmd.number<uint16_t>("--port");   // expected<optional<T>>
 * bool tls = cmd.isSet("--tls");
 * @endcode
 *
 * Values are copied, so the parser does not depend on argv's lifetime. A
 * value is always the next argument for an option, even when it starts with
 * '-' (so negative numbers work). Positional arguments are rejected.
 */
#pragma once

#include <charconv>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace sutra
{

class CommandLine
{
  public:
    /**
     * @brief Declares an option that takes a value.
     * @param names comma separated aliases, e.g. "--port,-p".
     * @param metavar placeholder shown in usage(), e.g. "N".
     */
    CommandLine& addOption(std::string names, std::string metavar,
                           std::string help)
    {
        return add(std::move(names), std::move(metavar), std::move(help), true);
    }

    /** @brief Declares a boolean flag (no value). */
    CommandLine& addFlag(std::string names, std::string help)
    {
        return add(std::move(names), {}, std::move(help), false);
    }

    /** @brief Parses `argv`; argv[0] is the program name and is skipped. */
    std::expected<void, std::string> parse(int argc, const char* const* argv)
    {
        values.clear();
        flags.clear();
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg = argv[i];
            std::optional<std::string> inlineValue;
            if (auto eq = arg.find('=');
                arg.starts_with("--") && eq != std::string_view::npos)
            {
                inlineValue = std::string(arg.substr(eq + 1));
                arg = arg.substr(0, eq);
            }
            const Spec* spec = find(arg);
            if (spec == nullptr)
            {
                return std::unexpected(
                    "unknown argument '" + std::string(arg) + "'");
            }
            if (!spec->takesValue)
            {
                if (inlineValue)
                {
                    return std::unexpected(
                        "option '" + spec->canonical() + "' takes no value");
                }
                flags[spec->canonical()] = true;
                continue;
            }
            if (inlineValue)
            {
                values[spec->canonical()] = std::move(*inlineValue);
                continue;
            }
            if (i + 1 >= argc)
            {
                return std::unexpected(
                    "option '" + spec->canonical() + "' needs a value");
            }
            values[spec->canonical()] = argv[++i];
        }
        return {};
    }

    /** @brief True when the flag, or an option with a value, was given. */
    bool isSet(std::string_view name) const
    {
        const Spec* spec = find(name);
        if (spec == nullptr)
        {
            return false;
        }
        return spec->takesValue ? values.contains(spec->canonical())
                                : flags.contains(spec->canonical());
    }

    /** @brief The value of an option, or nullopt when it was not given. */
    std::optional<std::string> value(std::string_view name) const
    {
        const Spec* spec = find(name);
        if (spec == nullptr)
        {
            return std::nullopt;
        }
        auto it = values.find(spec->canonical());
        if (it == values.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::string valueOr(std::string_view name, std::string fallback) const
    {
        return value(name).value_or(std::move(fallback));
    }

    /**
     * @brief The value of an option parsed as a number.
     * @return nullopt when the option was not given, an error message when it
     *         is not a number or does not fit into T.
     */
    template <class T>
    std::expected<std::optional<T>, std::string> number(
        std::string_view name) const
    {
        auto text = value(name);
        if (!text)
        {
            return std::optional<T>{};
        }
        T parsed{};
        const char* first = text->data();
        const char* last = first + text->size();
        auto [end, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc{} || end != last)
        {
            return std::unexpected(
                "option '" + std::string(name) +
                "' needs a number in range, got '" + *text + "'");
        }
        return std::optional<T>(parsed);
    }

    /** @brief Usage text built from the declared options. */
    std::string usage(std::string_view program) const
    {
        std::string out = "usage: " + std::string(program) + " [options]\n";
        for (const auto& spec : specs)
        {
            std::string left = "  " + spec.names;
            if (spec.takesValue)
            {
                left += " " + spec.metavar;
            }
            if (left.size() < 28)
            {
                left.resize(28, ' ');
            }
            else
            {
                left += "  ";
            }
            out += left + spec.help + "\n";
        }
        return out;
    }

  private:
    struct Spec
    {
        std::string names;
        std::string metavar;
        std::string help;
        bool takesValue = false;
        std::vector<std::string> aliases;

        const std::string& canonical() const
        {
            return aliases.front();
        }
    };

    CommandLine& add(std::string names, std::string metavar, std::string help,
                     bool takesValue)
    {
        Spec spec;
        spec.names = names;
        spec.metavar = std::move(metavar);
        spec.help = std::move(help);
        spec.takesValue = takesValue;
        std::string_view rest = names;
        while (!rest.empty())
        {
            auto comma = rest.find(',');
            auto part = rest.substr(0, comma);
            if (!part.empty())
            {
                spec.aliases.emplace_back(part);
            }
            rest = comma == std::string_view::npos ? std::string_view()
                                                   : rest.substr(comma + 1);
        }
        if (spec.aliases.empty())
        {
            throw std::invalid_argument("option declared without a name");
        }
        specs.push_back(std::move(spec));
        return *this;
    }

    const Spec* find(std::string_view name) const
    {
        for (const auto& spec : specs)
        {
            for (const auto& alias : spec.aliases)
            {
                if (alias == name)
                {
                    return &spec;
                }
            }
        }
        return nullptr;
    }

    std::vector<Spec> specs;
    std::map<std::string, std::string> values;
    std::map<std::string, bool> flags;
};

} // namespace sutra
