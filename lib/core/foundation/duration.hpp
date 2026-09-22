#pragma once

#include <chrono>
#include <concepts>
#include <numeric>

#include "types.hpp"

namespace daveos::core {


  // Integral durations only: conversion must be exact and leave kForever
  // reserved. No rounding, floating-point arithmetic, or allocation is used.
  template <typename Rep>
  concept DurationRep = std::integral<Rep> && sizeof(Rep) <= sizeof(Time);

  // Unsigned microseconds with the same count type as Time. Use it to pass a
  // value already measured in Time (a deadline difference, a parsed delay) to
  // a chrono-only API without a narrowing conversion.
  using Microseconds = std::chrono::duration<Time, std::micro>;

  template <DurationRep Rep, typename Period>
  constexpr Status to_microseconds(std::chrono::duration<Rep, Period> delay,
                                   Time& result) {
    if constexpr (std::signed_integral<Rep>) {
      if (delay.count() < 0) {
        return Status::invalid_argument;
      }
    }
    // Reduce the microsecond scale without forming Period::num * 1000000,
    // which can overflow even for a valid chrono period.
    constexpr Time common = std::gcd(Time{Period::den}, Time{1000000});
    constexpr Time divisor = Period::den / common;
    constexpr Time multiplier = 1000000 / common;
    const auto count = static_cast<Time>(delay.count());
    if (count % divisor != 0 ||
        count / divisor > (kForever - 1) / Period::num) {
      return Status::invalid_argument;
    }
    const Time scaled = count / divisor * Period::num;
    if (scaled > (kForever - 1) / multiplier) {
      return Status::invalid_argument;
    }
    result = scaled * multiplier;
    return Status::ok;
  }


}  // namespace daveos::core
