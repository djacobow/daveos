#pragma once

#include <array>
#include <span>
#include <string_view>

#include "service.h"

struct tcp_pcb;
namespace daveos::net {


struct TcpCallbacks;
// One-client, nonblocking byte stream. No DaveOS dependency or runtime heap.
// Call only from the same context as Service::poll(). Service must be
// initialized first and outlive this object; stop the server before stopping
// the service. A second client is reset. FIN/error/link loss discards the
// entire session.
class TcpServer {
 public:
  explicit TcpServer(Service& service, std::uint16_t port = 1000)
      : service_(service), port_(port) {}
  ~TcpServer();
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;
  // Start listening once IPv4 is ready and advance queued output. After link
  // loss, retry automatically when the service is ready again.
  void poll();
  void stop();
  bool connected() const { return client_ != nullptr; }
  std::uint32_t session() const { return session_; }
  std::uint32_t dropped_output() const { return dropped_; }
  std::size_t read(std::span<char> bytes);
  // Borrow the next contiguous RX range; valid until consumption, disconnect,
  // or another service/server call. consume() releases bytes and opens the TCP
  // window. Inspecting bytes alone does not acknowledge application
  // consumption.
  std::span<const char> peek() const;
  void consume(std::size_t size);
  // Copy all pieces atomically, or drop the complete record. Disconnected
  // output is not retained. Accepted data is delivered only while this session
  // survives.
  bool write(std::span<const std::string_view> pieces);

 private:
  friend struct TcpCallbacks;
  void Disconnect(bool abort);
  Service& service_;
  std::uint16_t port_;
  tcp_pcb* listener_ = nullptr;
  tcp_pcb* client_ = nullptr;
  std::array<char, 4096> rx_{};
  std::array<char, 8192> tx_{};
  std::size_t head_ = 0, received_ = 0, queued_ = 0;
  std::uint32_t session_ = 0, dropped_ = 0;
};


}  // namespace daveos::net
