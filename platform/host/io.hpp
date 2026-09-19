#pragma once

#include <cstdio>

#include "core/logging/log_format.hpp"

namespace daveos::platform::host {


  // A borrowed stdout sink with the standard prefix and one newline per record.
  // Flush each record for interactive terminals and pipes. This uses only
  // stdio, so it can also accompany the fake platform; no timer thread is
  // introduced.
  inline core::Subscriber stdout_subscriber() {
    return {nullptr, [](void*, const core::LogRecord& record) {
              core::LogPrefix prefix(record);
              const auto text = prefix.view();
              std::printf("%.*s%.*s\n", static_cast<int>(text.size()),
                          text.data(), static_cast<int>(record.message.size()),
                          record.message.data());
              std::fflush(stdout);
            }};
  }

  // Run a Scheduler or Application and translate its status into a process exit
  // code. Failure goes to stderr even when DaveOS logging is disabled.
  template <typename Runnable>
  int run(Runnable& runnable) {
    const auto status = runnable.run();
    if (status == core::Status::ok) {
      return 0;
    }
    std::fprintf(stderr, "DaveOS: %s\n", core::enum_name(status));
    return 1;
  }


}  // namespace daveos::platform::host
