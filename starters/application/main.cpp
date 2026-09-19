#include <cstdio>

#include "core/schedule/application.hpp"
#include "counter.hpp"
#include "platform/host/io.hpp"
#include "platform/host/platform.h"

namespace app {
  Counter counter;
}  // namespace app

int main() {
  daveos::platform::host::Platform platform;
  auto application = app::core::make_application<app::Event>(
      platform, app::core::ModuleList{&app::counter});
  const auto result = daveos::platform::host::run(application);
  if (result == 0) {
    std::puts("Three ticks completed");
  }
  return result;
}
