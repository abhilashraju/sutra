#include "sutra/graphql/parser.hpp"
#include "sutra/graphql/util.hpp"

#include <gtest/gtest.h>

using namespace sutra::graphql;

TEST(GraphqlParser, QueryWithVariables)
{
    Operation operation = Parser::parse(
        "query GetUser($userId: Int!) { user(id: $userId) { id name } }");

    EXPECT_EQ(operation.type, Operation::Type::Query);
    EXPECT_EQ(operation.name, "GetUser");
    ASSERT_EQ(operation.variableDefinitions.size(), 1u);
    EXPECT_EQ(operation.variableDefinitions[0].name, "userId");
    ASSERT_EQ(operation.selections.size(), 1u);
    EXPECT_EQ(operation.selections[0].name, "user");
    ASSERT_EQ(operation.selections[0].arguments.size(), 1u);
    EXPECT_EQ(operation.selections[0].arguments[0].value.value["$variable"],
              "userId");
    EXPECT_EQ(operation.selections[0].selections.size(), 2u);
}

TEST(GraphqlParser, LiteralArguments)
{
    Operation operation = Parser::parse(
        "mutation { create(name: \"Test User\", age: 25, ok: true) { id } }");

    EXPECT_EQ(operation.type, Operation::Type::Mutation);
    const auto& args = operation.selections[0].arguments;
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0].value.value, "Test User");
    EXPECT_EQ(args[1].value.value, 25);
    EXPECT_EQ(args[2].value.value, true);
}

TEST(GraphqlParser, DefaultValuesAndLists)
{
    Operation operation = Parser::parse(
        "query($input: Input = {enabled: true, ids: [1, 2, 3]}) { users { id } "
        "}");

    ASSERT_EQ(operation.variableDefinitions.size(), 1u);
    ASSERT_TRUE(operation.variableDefinitions[0].defaultValue.has_value());
    const auto& value = operation.variableDefinitions[0].defaultValue->value;
    EXPECT_EQ(value["enabled"], true);
    ASSERT_TRUE(value["ids"].is_array());
    EXPECT_EQ(value["ids"].size(), 3u);
}

TEST(GraphqlParser, Directives)
{
    Operation operation = Parser::parse(
        "query Q @skip(if: false) { users @include(if: true) { id } }");

    EXPECT_EQ(operation.directives.size(), 1u);
    ASSERT_EQ(operation.selections.size(), 1u);
    EXPECT_EQ(operation.selections[0].directives.size(), 1u);
}

TEST(GraphqlParser, NamedFragmentsExpand)
{
    Operation operation = Parser::parse(
        "query { user { ...UserFields } }"
        " fragment UserFields on User { id name email }");

    EXPECT_EQ(operation.fragments.count("UserFields"), 1u);
    ASSERT_EQ(operation.selections[0].selections.size(), 1u);
    EXPECT_FALSE(
        operation.selections[0].selections[0].fragmentSpreadName.empty());

    expandFragments(operation);
    const auto& fields = operation.selections[0].selections;
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0].name, "id");
    EXPECT_EQ(fields[1].name, "name");
    EXPECT_EQ(fields[2].name, "email");
}

TEST(GraphqlParser, InlineFragmentsExpandAtParseTime)
{
    Operation operation =
        Parser::parse("query { user { id ... on User { name email } } }");

    expandFragments(operation);
    const auto& fields = operation.selections[0].selections;
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[1].name, "name");
}

TEST(GraphqlParser, CircularFragmentsThrow)
{
    Operation operation = Parser::parse(
        "query { a { ...FragA } }"
        " fragment FragA on T { b { ...FragB } }"
        " fragment FragB on T { c { ...FragA } }");

    EXPECT_THROW(expandFragments(operation), std::runtime_error);
}

TEST(GraphqlParser, UnknownFragmentThrows)
{
    Operation operation = Parser::parse("query { a { ...Missing } }");
    EXPECT_THROW(expandFragments(operation), std::runtime_error);
}

TEST(GraphqlParser, InvalidSyntaxIsReported)
{
    std::string error;
    EXPECT_FALSE(Parser::validate("query { users { id name }", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(Parser::tryParse("{ a(b: ) }"));
}

TEST(GraphqlParser, DeepNestingIsRejected)
{
    std::string query = "query ";
    for (int i = 0; i < 100; ++i)
    {
        query += "{ a ";
    }
    for (int i = 0; i < 100; ++i)
    {
        query += "}";
    }
    EXPECT_FALSE(Parser::tryParse(query));
}
