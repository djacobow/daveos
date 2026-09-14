#pragma once

#include "core/command/source.hpp"
#include "core/logging/log_format.hpp"
#include "core/schedule/module.hpp"
#include "input.hpp"
#include "net/tcp_server.h"

namespace app {


// Plain TCP console: local terminal echo, shared command parsing and logging.
// Transport callbacks only buffer bytes; commands run in the scheduled task.
// Register this module's source/subscriber independently of UART and USB.
template <typename Event, typename Platform>
class TcpConsole final
    : public daveos::core::Module<TcpConsole<Event, Platform>, Event> {
 public:
  TcpConsole(Platform& platform, daveos::net::Service& service,
             std::uint16_t port = 1000)
      : server_(service, port), input_(platform) {}
  static constexpr const char* name() { return "tcp"; }
  static constexpr auto tasks() {
    return std::array{
        daveos::core::TaskDescriptor<TcpConsole>{"input", &TcpConsole::Poll}};
  }
  daveos::core::Status init(daveos::core::InitStage stage) {
    if (stage != daveos::core::InitStage::stage1)
      return daveos::core::Status::ok;
    return this->scheduler().schedule(*this, &TcpConsole::Poll, 1000,
                                      daveos::core::Mode::repeat);
  }
  daveos::core::CommandSource& command_source() { return source_; }
  daveos::core::Subscriber subscriber() {
    return {this, [](void* context, const daveos::core::LogRecord& record) {
              auto& self = *static_cast<TcpConsole*>(context);
              daveos::core::LogPrefix prefix(record);
              const std::array<std::string_view, 3> pieces{
                  prefix.view(), record.message, "\r\n"};
              self.server_.write(pieces);
            }};
  }
  void stop() { server_.stop(); }

 private:
  void Poll() {
    server_.poll();
    if (session_ != server_.session()) {
      session_ = server_.session();
      input_.reset();
    }
    // Bound work and stop after one complete line so a burst cannot fill the
    // line queue before dispatch gets a chance to consume it.
    Line line;
    for (std::size_t i = 0; i < 256; ++i) {
      std::array<char, 1> byte;
      if (!server_.read(byte)) break;
      input_.receive(byte[0]);
      if (!input_.pop(line)) continue;
      if (line.size)
        I_("> %.*s", static_cast<int>(line.size), line.bytes.data());
      source_.dispatch(line.view());
      break;
    }
  }
  daveos::net::TcpServer server_;
  Input<Platform> input_;
  daveos::core::CommandSource source_;
  std::uint32_t session_ = 0;
};


}  // namespace app
