#pragma once

#include <expected>
#include <string>

namespace sutra::graphql
{

// Human-readable error message carried by all Result failures.
using GraphQLError = std::string;

// Alias for std::expected<T, GraphQLError> used throughout the GraphQL stack.
template <typename T>
using Result = std::expected<T, GraphQLError>;

} // namespace sutra::graphql
