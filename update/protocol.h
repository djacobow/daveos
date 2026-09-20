#pragma once

#include "engine.h"

namespace daveos::update {


  inline constexpr std::uint32_t kProtocolMagic = 0x54534f44;
  enum class Operation : std::uint32_t {
    begin = 1,
    status,
    data,
    abort,
    reboot
  };
  const char* state_name(Engine::State state);

  struct Reboot {
    void* context = nullptr;
    // Accept by queuing a delayed reset, allowing the response to leave the
    // transport; never reset inline. No callback means explicitly unsupported.
    Status (*request)(void*) = nullptr;
  };

  // One framed byte stream, independent of sockets/UART/files. The v1 request
  // header is magic/version/opcode/payload-length (four LE uint32s). Replies
  // contain result, phase, ready, next offset, total, maximum chunk followed by
  // fixed terminated status[32] and phase[16] labels. No allocation.
  class Protocol {
   public:
    Protocol(Engine& engine, Reboot reboot = {})
        : engine_(engine), reboot_(reboot) {}

    std::size_t tick(std::span<const std::byte> input = {});
    std::span<const std::byte> reply() const;

    void sent() { sent_ = true; }

    void reset() { reset_ = true; }

    bool failed() const { return cs == State::failed; }

   private:
    enum class State { header, payload, reply, failed };
    Status Dispatch();
    void Reply(Status status);
    Engine& engine_;
    Reboot reboot_;
    std::array<std::byte, 16> header_{};
    std::array<std::byte, kBlockSize + 8> payload_{};
    std::array<std::byte, 88> reply_{};
    State cs = State::header;
    std::uint32_t opcode_ = 0, length_ = 0;
    std::size_t filled_ = 0;
    bool sent_ = false, reset_ = false;
  };


}  // namespace daveos::update
