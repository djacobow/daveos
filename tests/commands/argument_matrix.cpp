#include "core/command/arguments.hpp"
#include "support.hpp"

namespace core = daveos::core;

namespace {
  template <typename T>
  struct Receiver {
    std::size_t calls = 0;
    std::optional<T> received;

    core::Status Required(T value) {
      ++calls;
      received = value;
      return core::Status::ok;
    }

    core::Status Optional(std::optional<T> value) {
      ++calls;
      received = value;
      return core::Status::ok;
    }
  };

  // Exercise public descriptors, including their count guard and invocation;
  // do not call internal conversion helpers directly.
  template <typename M>
  void Reject(M& module, const core::CommandDescriptor<M>& descriptor,
              core::CommandArguments args, bool count_error = false) {
    const auto calls = module.calls;
    core::ArgumentError error{};
    CHECK(descriptor.callback(module, args, descriptor, error) ==
          core::Status::invalid_argument);
    CHECK(module.calls == calls);
    REQUIRE(error.reason != nullptr);
    if (count_error) {
      CHECK(error.argument == nullptr);
    } else {
      REQUIRE(error.argument != nullptr);
      CHECK(std::string_view(error.argument->name) == "value");
    }
  }

  template <typename T>
  void Accept(Receiver<T>& module,
              const core::CommandDescriptor<Receiver<T>>& descriptor,
              std::string_view text, T expected) {
    const auto calls = module.calls;
    const std::array args{text};
    core::ArgumentError error{};
    REQUIRE(descriptor.callback(module, args, descriptor, error) ==
            core::Status::ok);
    CHECK(module.calls == calls + 1);
    REQUIRE(module.received.has_value());
    CHECK(*module.received == expected);
    CHECK(error.reason == nullptr);
  }

  template <typename T, typename Check>
  void Both(Check check) {
    using M = Receiver<T>;
    constexpr auto required =
        core::command<&M::Required>("required", "Required", core::arg("value"));
    constexpr auto optional =
        core::command<&M::Optional>("optional", "Optional", core::arg("value"));
    M module;
    Reject(module, required, {}, true);
    for (const auto& descriptor : {required, optional}) {
      check(module, descriptor);
      const std::array<std::string_view, 2> extra{"1", "2"};
      Reject(module, descriptor, extra, true);
    }
    core::ArgumentError error{};
    const auto calls = module.calls;
    CHECK(optional.callback(module, {}, optional, error) == core::Status::ok);
    CHECK(module.calls == calls + 1);
    CHECK_FALSE(module.received.has_value());
    CHECK(error.reason == nullptr);
  }
}  // namespace

TEMPLATE_TEST_CASE(
    "required and optional integers cover every width and syntax", "[typed]",
    std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
    std::uint32_t, std::int64_t, std::uint64_t) {
  using T = TestType;
  Both<T>([](auto& module, const auto& descriptor) {
    const auto low = std::numeric_limits<T>::min();
    const auto high = std::numeric_limits<T>::max();
    Accept(module, descriptor, std::to_string(low), low);
    Accept(module, descriptor, std::to_string(high), high);
    for (auto text : {"0", "+0", "000", "0x0", "0X0", "0b0", "0B0"}) {
      Accept(module, descriptor, text, T{0});
    }
    for (auto text : {"10", "+10", "010", "0xa", "0XA", "+0xA", "0b1010",
                      "0B1010", "+0b1010"}) {
      Accept(module, descriptor, text, T{10});
    }
    for (std::int32_t number = -130; number <= 257; ++number) {
      const auto text = std::to_string(number);
      CAPTURE(text);
      if (std::in_range<T>(number)) {
        Accept(module, descriptor, text, static_cast<T>(number));
      } else {
        const std::array args{std::string_view(text)};
        Reject(module, descriptor, args);
      }
    }
    if constexpr (std::is_signed_v<T>) {
      for (auto text : {"-10", "-0xa", "-0b1010"}) {
        Accept(module, descriptor, text, static_cast<T>(-10));
      }
      if constexpr (sizeof(T) < sizeof(std::int64_t)) {
        const auto below = std::to_string(static_cast<std::int64_t>(low) - 1);
        const std::array args{std::string_view(below)};
        Reject(module, descriptor, args);
      } else {
        const std::array<std::string_view, 1> args{"-9223372036854775809"};
        Reject(module, descriptor, args);
      }
    } else {
      for (auto text : {"-0", "-1", "-0x1", "-0b1"}) {
        const std::array<std::string_view, 1> args{text};
        Reject(module, descriptor, args);
      }
    }
    if constexpr (sizeof(T) < sizeof(std::uint64_t) || std::is_signed_v<T>) {
      const auto above = std::to_string(static_cast<std::uint64_t>(high) + 1);
      const std::array args{std::string_view(above)};
      Reject(module, descriptor, args);
    }
    for (auto text : {"",
                      " ",
                      " 1",
                      "1 ",
                      "1\t",
                      "1.0",
                      "1e2",
                      "0x",
                      "0b",
                      "0b2",
                      "0xg",
                      "12tail",
                      "+",
                      "-",
                      "--1",
                      "++1",
                      "+-1",
                      "-+1",
                      "0x-1",
                      "0b+1",
                      "1,000",
                      "18446744073709551616",
                      "0x10000000000000000"}) {
      CAPTURE(text);
      const std::array<std::string_view, 1> args{text};
      Reject(module, descriptor, args);
    }
    const std::array args{std::string_view("1\0x", 3)};
    Reject(module, descriptor, args);
  });
}

TEMPLATE_TEST_CASE(
    "required and optional floats cover syntax and representable boundaries",
    "[typed]", float, double) {
  using T = TestType;
  Both<T>([](auto& module, const auto& descriptor) {
    for (auto text : {"1.25", "+1.25", "125e-2", "0.125E+1"}) {
      Accept(module, descriptor, text, T{1.25});
    }
    for (auto text : {"-1.25", "-125e-2"}) {
      Accept(module, descriptor, text, T{-1.25});
    }
    Accept(module, descriptor, ".5", T{0.5});
    Accept(module, descriptor, "1.", T{1});
    Accept(module, descriptor, "-0", T{0});
    CHECK(std::signbit(*module.received));
    for (T value :
         {std::numeric_limits<T>::lowest(), std::numeric_limits<T>::max(),
          std::numeric_limits<T>::min(),
          std::numeric_limits<T>::denorm_min()}) {
      std::array<char, 128> buffer{};
      const auto encoded =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
      REQUIRE(encoded.ec == std::errc{});
      Accept(module, descriptor, std::string_view(buffer.data(), encoded.ptr),
             value);
    }
    for (auto text :
         {"",    " ",   " 1",       "1 ",        "1x",  "1,25", "0x1p2",
          "+",   "-",   "++1",      "--1",       "+-1", "-+1",  "1e",
          "1e+", "1e-", "1e9999",   "1e-9999",   "nan", "NaN",  "nan(1)",
          "inf", "INF", "infinity", "-infinity", "+inf"}) {
      CAPTURE(text);
      const std::array<std::string_view, 1> args{text};
      Reject(module, descriptor, args);
    }
  });
}

TEMPLATE_TEST_CASE(
    "numeric bounds apply to required and supplied optional values", "[typed]",
    std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
    std::uint32_t, float) {
  using T = TestType;
  using M = Receiver<T>;
  constexpr auto bounded = core::arg("value").range(T{1}, T{3});
  constexpr auto lower = core::arg("value").min(T{1});
  constexpr auto upper = core::arg("value").max(T{3});
  constexpr auto equal = core::arg("value").range(T{2}, T{2});
  constexpr std::array descriptors{
      core::command<&M::Required>("both", "Bounds", bounded),
      core::command<&M::Optional>("both", "Bounds", bounded),
      core::command<&M::Required>("low", "Minimum", lower),
      core::command<&M::Optional>("low", "Minimum", lower),
      core::command<&M::Required>("high", "Maximum", upper),
      core::command<&M::Optional>("high", "Maximum", upper),
      core::command<&M::Required>("equal", "Single value", equal),
      core::command<&M::Optional>("equal", "Single value", equal)};
  for (const auto& descriptor : descriptors) {
    M module;
    for (std::uint32_t value = 0; value <= 4; ++value) {
      const auto text = std::to_string(value);
      const std::string_view name(descriptor.name);
      const bool valid = name == "both"   ? value >= 1 && value <= 3
                         : name == "low"  ? value >= 1
                         : name == "high" ? value <= 3
                                          : value == 2;
      if (valid) {
        Accept(module, descriptor, text, static_cast<T>(value));
      } else {
        const std::array args{std::string_view(text)};
        Reject(module, descriptor, args);
      }
    }
    if (descriptor.required == 0) {
      core::ArgumentError error{};
      CHECK(descriptor.callback(module, {}, descriptor, error) ==
            core::Status::ok);
      CHECK_FALSE(module.received.has_value());
    } else {
      Reject(module, descriptor, {}, true);
    }
  }
}

TEST_CASE(
    "optional booleans distinguish absent false and invalid with every alias") {
  using M = Receiver<bool>;
  constexpr std::array descriptors{
      core::command<&M::Required>("strict", "Strict", core::arg("value")),
      core::command<&M::Optional>("strict", "Strict", core::arg("value")),
      core::command<&M::Required>("friendly", "Friendly",
                                  core::arg("value").friendly()),
      core::command<&M::Optional>("friendly", "Friendly",
                                  core::arg("value").friendly())};
  constexpr std::array<std::pair<std::string_view, bool>, 14> aliases{
      {{"true", true},
       {"false", false},
       {"1", true},
       {"0", false},
       {"on", true},
       {"off", false},
       {"yes", true},
       {"no", false},
       {"enable", true},
       {"disable", false},
       {"high", true},
       {"low", false},
       {"set", true},
       {"clear", false}}};
  for (const auto& descriptor : descriptors) {
    M module;
    for (const auto& [text, expected] : aliases) {
      std::string uppercase(text);
      std::string mixed(text);
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] >= 'a' && text[i] <= 'z') {
          uppercase[i] -= 'a' - 'A';
          if (i % 2 == 0) {
            mixed[i] -= 'a' - 'A';
          }
        }
      }
      for (std::string_view spelling :
           {text, std::string_view(uppercase), std::string_view(mixed)}) {
        if (std::string_view(descriptor.name) == "friendly" ||
            spelling == "true" || spelling == "false") {
          Accept(module, descriptor, spelling, expected);
        } else {
          const std::array args{spelling};
          Reject(module, descriptor, args);
        }
      }
    }
    for (auto text :
         {"", "flase", "2", "-1", "enabled", "disabled", "true ", " false"}) {
      const std::array<std::string_view, 1> args{text};
      Reject(module, descriptor, args);
    }
    if (descriptor.required == 0) {
      core::ArgumentError error{};
      CHECK(descriptor.callback(module, {}, descriptor, error) ==
            core::Status::ok);
      CHECK_FALSE(module.received.has_value());
    } else {
      Reject(module, descriptor, {}, true);
    }
  }
}

TEST_CASE(
    "text parameters preserve empty and whitespace values including optional "
    "text") {
  Both<std::string_view>([](auto& module, const auto& descriptor) {
    for (std::string_view text :
         {"", "one", "two words", " leading", "trailing ", "a\"b", "a\\b"}) {
      Accept(module, descriptor, text, text);
    }
  });
}

TEMPLATE_TEST_CASE(
    "floating range boundaries include endpoints but exclude adjacent outside "
    "values",
    "[typed]", float) {
  using T = TestType;
  using M = Receiver<T>;
  constexpr auto bounds = core::arg("value").range(T{1}, T{2});
  constexpr std::array descriptors{
      core::command<&M::Required>("required", "Required", bounds),
      core::command<&M::Optional>("optional", "Optional", bounds)};
  for (const auto& descriptor : descriptors) {
    M module;
    for (T value :
         {T{1}, T{2}, std::nextafter(T{1}, T{2}), std::nextafter(T{2}, T{1})}) {
      std::array<char, 128> buffer{};
      const auto encoded =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
      REQUIRE(encoded.ec == std::errc{});
      Accept(module, descriptor, std::string_view(buffer.data(), encoded.ptr),
             value);
    }
    for (T value : {std::nextafter(T{1}, T{0}), std::nextafter(T{2}, T{3})}) {
      std::array<char, 128> buffer{};
      const auto encoded =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
      REQUIRE(encoded.ec == std::errc{});
      const std::array args{std::string_view(buffer.data(), encoded.ptr)};
      Reject(module, descriptor, args);
    }
  }
}
