#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "core/platform/platform.hpp"

namespace daveos::core {


// Locale-independent ASCII folding; argument text is never folded.
constexpr char Fold(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool EqualName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (Fold(a[i]) != Fold(b[i])) return false;
  return true;
}

// Validate static registration names; optional null entries represent modules
// with no commands when checking prefixes.
constexpr bool UniqueNames(std::span<const char* const> names,
                           bool allow_null = false) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!names[i]) {
      if (allow_null) continue;
      return false;
    }
    if (!*names[i]) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (names[j] && EqualName(names[i], names[j])) return false;
  }
  return true;
}

// Routing names are nonempty ASCII identifiers, with hyphens permitted.
constexpr bool ValidCommandName(const char* name) {
  if (!name || !*name) return false;
  for (; *name; ++name) {
    char c = Fold(*name);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-'))
      return false;
  }
  return true;
}

struct NameMatch {
  Status status = Status::not_found;
  std::size_t index = 0;
};

// Exact match wins; otherwise accept one unique prefix. name(index) returns
// a string view for each candidate. Registration must reject duplicate names.
// An empty query never matches, including an explicitly quoted empty token.
template <typename Names>
constexpr NameMatch lazy_match(std::string_view query, std::size_t count,
                               Names name) {
  NameMatch result;
  if (query.empty()) return result;
  for (std::size_t i = 0; i < count; ++i) {
    std::string_view candidate = name(i);
    if (EqualName(query, candidate)) return {Status::ok, i};
    if (query.size() < candidate.size() &&
        EqualName(query, candidate.substr(0, query.size()))) {
      result = {result.status == Status::not_found ? Status::ok
                                                   : Status::ambiguous_match,
                i};
    }
  }
  return result;
}


}  // namespace daveos::core
