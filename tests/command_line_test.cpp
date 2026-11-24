#include "sutra/command_line.hpp"

#include <gtest/gtest.h>

using sutra::CommandLine;

namespace
{

CommandLine makeParser()
{
    CommandLine cmd;
    cmd.addOption("--port,-p", "N", "port");
    cmd.addOption("--bus", "NAME", "bus");
    cmd.addFlag("--tls,-t", "serve https");
    return cmd;
}

std::expected<void, std::string> run(CommandLine& cmd,
                                     std::vector<const char*> args)
{
    args.insert(args.begin(), "prog");
    return cmd.parse(static_cast<int>(args.size()), args.data());
}

} // namespace

TEST(CommandLine, OptionsFlagsAndAliases)
{
    auto cmd = makeParser();
    ASSERT_TRUE(run(cmd, {"-p", "9000", "--tls", "--bus", "session"}));
    EXPECT_EQ(cmd.value("--port"), "9000");
    EXPECT_EQ(cmd.value("-p"), "9000");
    EXPECT_EQ(cmd.value("--bus"), "session");
    EXPECT_TRUE(cmd.isSet("--tls"));
    EXPECT_TRUE(cmd.isSet("-t"));
    EXPECT_TRUE(cmd.isSet("--port"));
}

TEST(CommandLine, AbsentValuesAndDefaults)
{
    auto cmd = makeParser();
    ASSERT_TRUE(run(cmd, {}));
    EXPECT_FALSE(cmd.isSet("--tls"));
    EXPECT_FALSE(cmd.isSet("--port"));
    EXPECT_FALSE(cmd.value("--bus"));
    EXPECT_EQ(cmd.valueOr("--bus", "system"), "system");
    auto port = cmd.number<uint16_t>("--port");
    ASSERT_TRUE(port);
    EXPECT_FALSE(*port);
    EXPECT_FALSE(cmd.isSet("--undeclared"));
}

TEST(CommandLine, FlagBetweenOptionsDoesNotShiftArguments)
{
    // coroserver's pair-wise parser mis-pairs this case.
    auto cmd = makeParser();
    ASSERT_TRUE(run(cmd, {"--tls", "--port", "1", "--bus", "none"}));
    EXPECT_EQ(cmd.value("--port"), "1");
    EXPECT_EQ(cmd.value("--bus"), "none");
    EXPECT_TRUE(cmd.isSet("--tls"));
}

TEST(CommandLine, EqualsSyntax)
{
    auto cmd = makeParser();
    ASSERT_TRUE(run(cmd, {"--port=8080", "--bus=a=b"}));
    EXPECT_EQ(cmd.value("--port"), "8080");
    EXPECT_EQ(cmd.value("--bus"), "a=b");
    EXPECT_FALSE(run(cmd, {"--tls=1"}));
}

TEST(CommandLine, Errors)
{
    auto cmd = makeParser();
    auto unknown = run(cmd, {"--nope"});
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().find("unknown argument '--nope'"),
              std::string::npos);

    auto missing = run(cmd, {"--port"});
    ASSERT_FALSE(missing);
    EXPECT_NE(missing.error().find("needs a value"), std::string::npos);

    // Positional arguments are rejected.
    EXPECT_FALSE(run(cmd, {"file.txt"}));
}

TEST(CommandLine, ValueMayStartWithDash)
{
    CommandLine cmd;
    cmd.addOption("--offset", "N", "signed offset");
    ASSERT_TRUE(run(cmd, {"--offset", "-5"}));
    auto offset = cmd.number<int>("--offset");
    ASSERT_TRUE(offset);
    EXPECT_EQ(**offset, -5);
}

TEST(CommandLine, NumberValidation)
{
    auto cmd = makeParser();
    ASSERT_TRUE(run(cmd, {"-p", "65535"}));
    EXPECT_EQ(**cmd.number<uint16_t>("--port"), 65535);

    ASSERT_TRUE(run(cmd, {"-p", "65536"}));
    EXPECT_FALSE(cmd.number<uint16_t>("--port"));

    ASSERT_TRUE(run(cmd, {"-p", "12abc"}));
    EXPECT_FALSE(cmd.number<uint16_t>("--port"));

    ASSERT_TRUE(run(cmd, {"-p", ""}));
    EXPECT_FALSE(cmd.number<uint16_t>("--port"));
}

TEST(CommandLine, ParseResetsPreviousState)
{
    auto cmd = makeParser();
    ASSERT_TRUE(run(cmd, {"--tls", "--port", "1"}));
    ASSERT_TRUE(run(cmd, {}));
    EXPECT_FALSE(cmd.isSet("--tls"));
    EXPECT_FALSE(cmd.isSet("--port"));
}

TEST(CommandLine, UsageListsOptions)
{
    auto text = makeParser().usage("prog");
    EXPECT_NE(text.find("usage: prog"), std::string::npos);
    EXPECT_NE(text.find("--port,-p N"), std::string::npos);
    EXPECT_NE(text.find("--tls,-t"), std::string::npos);
    EXPECT_NE(text.find("serve https"), std::string::npos);
}
