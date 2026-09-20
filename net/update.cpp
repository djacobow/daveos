#include "update.h"

namespace daveos::net {


  void UpdateServer::stop() {
    engine_.abort();
    server_.stop();
    protocol_.reset();
  }

  void UpdateServer::poll() {
    if (!engine_.enabled()) {
      stop();
      return;
    }
    server_.poll();
    if (server_.session() != session_) {
      session_ = server_.session();
      engine_.abort();
      protocol_.reset();
    }
    auto bytes = server_.peek();
    auto consumed = protocol_.tick(std::as_bytes(bytes));
    if (consumed) {
      server_.consume(consumed);
    }
    auto reply = protocol_.reply();
    if (!reply.empty()) {
      std::array pieces{std::string_view(
          reinterpret_cast<const char*>(reply.data()), reply.size())};
      if (server_.write(pieces)) {
        protocol_.sent();
      }
    }
    if (protocol_.failed()) {
      stop();
    }
  }


}  // namespace daveos::net
