#pragma once

#include <cinttypes>

#include "core/schedule/module.hpp"
#include "storage/transfer.h"
#include "tcp_server.h"

namespace daveos::net {


  // Optional file service. Kept separate from the console and OTA engine.
  // One-shot task rearming allows filesystem calls to yield without imposing
  // a periodic-task health deadline on a slow SD write. Network, filesystem
  // and platform outlive this object. Before destruction, stop() and continue
  // scheduler dispatch until stopped(); stop never performs filesystem I/O.
  template <typename Platform, typename Event = core::NoEvent>
  class FileTransferModule
      : public core::Module<FileTransferModule<Platform, Event>, Event> {
   public:
    FileTransferModule(Platform& platform, Service& network,
                       const storage::FileAccess& access,
                       std::uint16_t port = 1002)
        : platform_(platform), server_(network, port), transfer_(access) {}

    static constexpr const char* name() { return "files"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(FileTransferModule, Poll)};
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage2
                 ? this->template schedule<&FileTransferModule::Poll>(
                       std::chrono::milliseconds{1})
                 : core::Status::ok;
    }

    void stop() { stopping_ = true; }

    bool stopped() const { return stopped_; }

   private:
    void Session() {
      if (server_.session() != session_) {
        session_ = server_.session();
        if (transfer_.busy() || !transfer_.reply().empty()) {
          transfer_.disconnect();
        }
      }
    }

    void Poll() {
      if (stopping_) {
        server_.stop();
        transfer_.disconnect();
      } else {
        server_.poll();
      }
      Session();
      if (!stopping_) {
        // Copy/consume before calling FatFs: a yielded network poll can
        // invalidate a borrowed TCP receive span or disconnect the client.
        const auto consumed =
            transfer_.feed(std::as_bytes(server_.peek()), platform_.now());
        server_.consume(consumed);
      }
      transfer_.tick(platform_.now());
      transfer_.completed_at(platform_.now());
      Session();
      if (transfer_.failures() != failures_) {
        failures_ = transfer_.failures();
        E_("File transfer: %s", enum_name(transfer_.status()));
        if (transfer_.status() == storage::TransferStatus::cleanup_failed) {
          E_("Inspect/remove incomplete file: %s", transfer_.path());
        }
      }
      if (!stopping_ && server_.connected() && !transfer_.reply().empty()) {
        const auto reply = transfer_.reply();
        const std::array pieces{std::string_view(
            reinterpret_cast<const char*>(reply.data()), reply.size())};
        if (server_.write(pieces)) {
          transfer_.sent(platform_.now());
        }
      }
      stopped_ = stopping_ && !transfer_.busy();
      if (!stopped_) {
        (void)this->template schedule<&FileTransferModule::Poll>(
            std::chrono::milliseconds{1});
      }
    }

    Platform& platform_;
    TcpServer server_;
    storage::FileTransfer transfer_;
    std::uint32_t session_ = 0, failures_ = 0;
    bool stopping_ = false, stopped_ = false;
  };


}  // namespace daveos::net
