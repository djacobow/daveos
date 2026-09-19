#pragma once

#include "console/module.hpp"
#include "core/logging/log_format.hpp"
#include "net/tcp_server.h"

namespace daveos::console {


  // Plain TCP console: local terminal echo, shared command parsing and logging.
  // Transport callbacks only buffer bytes; commands run in the scheduled task.
  // Register this module's source/subscriber independently of UART and USB.
  template <typename Event, typename Platform>
  class TcpConsole final
      : public daveos::console::Module<TcpConsole<Event, Platform>, Event> {
   public:
    TcpConsole(Platform& platform, daveos::net::Service& service,
               std::uint16_t port = 1000)
        : server_(service, port), input_(platform) {}

    static constexpr const char* name() { return "tcp"; }

    void output(const daveos::core::LogRecord& record) {
      daveos::core::LogPrefix prefix(record);
      const std::array<std::string_view, 3> pieces{prefix.view(),
                                                   record.message, "\r\n"};
      server_.write(pieces);
    }

    std::uint32_t take_dropped() { return input_.take_dropped(); }

    void stop() { server_.stop(); }

    bool poll_line(daveos::console::Line& line) {
      server_.poll();
      if (session_ != server_.session()) {
        session_ = server_.session();
        input_.reset();
      }
      std::size_t budget = 256;
      while (budget) {
        const auto bytes = server_.peek();
        if (bytes.empty()) {
          break;
        }
        const auto consumed =
            input_.consume(bytes.first(std::min(bytes.size(), budget)), line);
        server_.consume(consumed.bytes);
        budget -= consumed.bytes;
        if (consumed.complete) {
          return true;
        }
      }
      return false;
    }

   private:
    daveos::net::TcpServer server_;
    daveos::console::Input<Platform> input_;
    std::uint32_t session_ = 0;
  };


}  // namespace daveos::console
