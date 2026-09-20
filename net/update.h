#pragma once

#include "tcp_server.h"
#include "update/protocol.h"

namespace daveos::net {


  // Optional TCP adapter; separate from the text console, one client only.
  // Poll in the same scheduler context as Service/TcpServer. Disconnection
  // aborts unfinished work; no transport automatically enables updating.
  class UpdateServer {
   public:
    UpdateServer(Service& network, update::Engine& engine,
                 update::Reboot reboot = {}, std::uint16_t port = 1001)
        : server_(network, port), engine_(engine), protocol_(engine, reboot) {}

    void poll();
    void stop();

   private:
    TcpServer server_;
    update::Engine& engine_;
    update::Protocol protocol_;
    std::uint32_t session_ = 0;
  };


}  // namespace daveos::net
