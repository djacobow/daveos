#include "tcp_server.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include "lwip/tcp.h"
}

namespace daveos::net {


struct TcpCallbacks {
  static err_t Accept(void* context, tcp_pcb* client, err_t error) {
    auto& self = *static_cast<TcpServer*>(context);
    if (error != ERR_OK || self.client_) {
      tcp_abort(client);
      return ERR_ABRT;
    }
    self.client_ = client;
    ++self.session_;
    tcp_arg(client, &self);
    tcp_recv(client, Receive);
    tcp_err(client, Error);
    tcp_nagle_disable(client);
    return ERR_OK;
  }

  static err_t Receive(void* context, tcp_pcb*, pbuf* packet, err_t) {
    auto& self = *static_cast<TcpServer*>(context);
    if (!packet) {
      self.Disconnect(true);
      return ERR_ABRT;
    }
    if (packet->tot_len > self.rx_.size() - self.received_) return ERR_MEM;
    const auto tail = (self.head_ + self.received_) % self.rx_.size();
    const auto first =
        std::min<std::size_t>(packet->tot_len, self.rx_.size() - tail);
    pbuf_copy_partial(packet, self.rx_.data() + tail, static_cast<u16_t>(first),
                      0);
    pbuf_copy_partial(packet, self.rx_.data(), packet->tot_len - first,
                      static_cast<u16_t>(first));
    self.received_ += packet->tot_len;
    pbuf_free(packet);
    return ERR_OK;
  }

  static void Error(void* context, err_t) {
    static_cast<TcpServer*>(context)->Disconnect(false);  // PCB already freed.
  }
};

TcpServer::~TcpServer() { stop(); }

void TcpServer::Disconnect(bool abort) {
  if (client_) {
    auto* client = client_;
    client_ = nullptr;
    if (abort) {
      tcp_arg(client, nullptr);
      tcp_err(client, nullptr);
      tcp_abort(client);
    }
    ++session_;
  }
  head_ = received_ = queued_ = 0;
}

void TcpServer::stop() {
  Disconnect(true);
  if (listener_) {
    tcp_close(listener_);  // Listening PCBs close without allocating a FIN.
    listener_ = nullptr;
  }
}

void TcpServer::poll() {
  if (service_.snapshot().state != State::ready) {
    stop();
    return;
  }
  if (!listener_) {
    auto* pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, port_) != ERR_OK) {
      tcp_abort(pcb);
      return;
    }
    listener_ = tcp_listen_with_backlog(pcb, 1);
    if (!listener_) {
      tcp_abort(pcb);
      return;
    }
    tcp_arg(listener_, this);
    tcp_accept(listener_, TcpCallbacks::Accept);
  }
  if (!client_ || !queued_) return;
  const auto size = static_cast<u16_t>(
      std::min<std::size_t>({queued_, tcp_sndbuf(client_), TCP_MSS}));
  if (!size ||
      tcp_write(client_, tx_.data(), size, TCP_WRITE_FLAG_COPY) != ERR_OK)
    return;
  queued_ -= size;
  std::memmove(tx_.data(), tx_.data() + size, queued_);
  tcp_output(client_);
}

std::span<const char> TcpServer::peek() const {
  return {rx_.data() + head_, std::min(received_, rx_.size() - head_)};
}

void TcpServer::consume(std::size_t size) {
  size = std::min(size, received_);
  head_ = (head_ + size) % rx_.size();
  received_ -= size;
  if (client_ && size) tcp_recved(client_, static_cast<u16_t>(size));
}

std::size_t TcpServer::read(std::span<char> bytes) {
  const auto size = std::min(bytes.size(), received_);
  std::size_t copied = 0;
  while (copied < size) {
    const auto chunk = peek().first(std::min(peek().size(), size - copied));
    std::copy(chunk.begin(), chunk.end(), bytes.begin() + copied);
    copied += chunk.size();
    consume(chunk.size());
  }
  return size;
}

bool TcpServer::write(std::span<const std::string_view> pieces) {
  if (!client_) return false;
  auto available = tx_.size() - queued_;
  for (auto piece : pieces) {
    if (piece.size() > available) {
      ++dropped_;
      return false;
    }
    available -= piece.size();
  }
  for (auto piece : pieces) {
    std::copy(piece.begin(), piece.end(), tx_.begin() + queued_);
    queued_ += piece.size();
  }
  return true;
}


}  // namespace daveos::net
