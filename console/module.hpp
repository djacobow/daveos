#pragma once

#include <inttypes.h>

#include "core/command/source.hpp"
#include "core/schedule/module.hpp"
#include "input.hpp"

namespace daveos::console {


  // Shared console task and registration. Derived supplies name(),
  // poll_line(Line&), take_dropped(), and output(LogRecord). Transport work
  // stays in Derived; tokenization remains exclusively in CommandDispatcher.
  // One line per invocation, polled every millisecond once dispatch starts.
  template <typename Derived, typename Event>
  class Module : public core::Module<Derived, Event> {
   public:
    static constexpr auto tasks() {
      return std::array{core::periodic_task<Derived>(
          "input", static_cast<void (Derived::*)()>(&Module::Poll),
          std::chrono::milliseconds{1})};
    }

    core::CommandSource& command_source() { return source_; }

    core::Subscriber subscriber() {
      return {static_cast<Derived*>(this),
              [](void* context, const core::LogRecord& record) {
                static_cast<Derived*>(context)->output(record);
              }};
    }

   private:
    void Poll() {
      auto& transport = static_cast<Derived&>(*this);
      [[maybe_unused]] const auto dropped = transport.take_dropped();
      if (dropped) {
        W_("Dropped %" PRIu32 " input lines/errors", dropped);
      }
      Line line;
      if (!transport.poll_line(line)) {
        return;
      }
      if (line.size) {
        I_("> %.*s", static_cast<int>(line.size), line.bytes.data());
      }
      source_.dispatch(line.view());
    }

    core::CommandSource source_;
  };


}  // namespace daveos::console
