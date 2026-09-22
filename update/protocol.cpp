#include "protocol.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "util/wire.h"

namespace daveos::update {


  namespace wire = util::wire;

  const char* state_name(Engine::State state) {
    switch (state) {
      case Engine::State::disabled:
        return "disabled";
      case Engine::State::idle:
        return "idle";
      case Engine::State::invalidating:
        return "invalidating";
      case Engine::State::receiving:
        return "receiving";
      case Engine::State::writing:
        return "writing";
      case Engine::State::verifying:
        return "verifying";
      case Engine::State::committing:
        return "committing";
      case Engine::State::done:
        return "done";
      case Engine::State::aborting:
        return "aborting";
      case Engine::State::failed:
        return "failed";
    }
    return "unknown";
  }

  Status Protocol::Dispatch() {
    if (engine_.reserved()) {
      return Status::busy;
    }
    auto data = std::span(payload_).first(length_);
    switch (static_cast<Operation>(opcode_)) {
      case Operation::begin:
        return engine_.begin(data);
      case Operation::data: {
        if (length_ <= 8) {
          return Status::invalid_argument;
        }
        return engine_.chunk(wire::read32(data, 0), data.subspan(8),
                             wire::read32(data, 4));
      }
      case Operation::status:
        return length_ ? Status::invalid_argument : engine_.status();
      case Operation::abort: {
        if (length_) {
          return Status::invalid_argument;
        }
        engine_.abort();
        return Status::ok;
      }
      case Operation::reboot: {
        if (length_) {
          return Status::invalid_argument;
        }
        if (engine_.state() != Engine::State::done) {
          return Status::not_running;
        }
        return reboot_.request ? reboot_.request(reboot_.context)
                               : Status::unsupported;
      }
    }
    return Status::unsupported;
  }

  void Protocol::Reply(Status status) {
    reply_.fill(std::byte{0});
    wire::write32(reply_, 0, kProtocolMagic);
    wire::write32(reply_, 4, 1);
    wire::write32(reply_, 8, opcode_ | 0x80000000);
    wire::write32(reply_, 12, reply_.size() - 16);
    wire::write32(reply_, 16, static_cast<std::uint32_t>(status));
    wire::write32(reply_, 20, static_cast<std::uint32_t>(engine_.state()));
    wire::write32(reply_, 24, !engine_.reserved() && engine_.ready());
    wire::write32(reply_, 28, engine_.next_offset());
    wire::write32(reply_, 32, engine_.total());
    wire::write32(reply_, 36, kBlockSize);
    auto result = std::string_view(enum_name(status));
    auto phase = std::string_view(state_name(engine_.state()));
    std::memcpy(reply_.data() + 40, result.data(),
                std::min<std::size_t>(31, result.size()));
    std::memcpy(reply_.data() + 72, phase.data(),
                std::min<std::size_t>(15, phase.size()));
  }

  std::span<const std::byte> Protocol::reply() const {
    return machine_.state() == State::reply ? std::span<const std::byte>(reply_)
                                            : std::span<const std::byte>{};
  }

  std::size_t Protocol::tick(std::span<const std::byte> input) {
    std::size_t count = 0;
    (void)machine_.tick(*this, input, count);
    return count;
  }

  void Protocol::Step(State cs, State& ns, std::span<const std::byte> input,
                      std::size_t& count) {
    switch (cs) {
      case State::header:
      case State::payload:
      case State::reply:
      case State::failed: {
        if (reset_) {
          reset_ = sent_ = false;
          filled_ = 0;
          ns = State::header;
          break;
        }
        switch (cs) {
          case State::header: {
            count = std::min(input.size(), header_.size() - filled_);
            std::copy_n(input.begin(), count, header_.begin() + filled_);
            filled_ += count;
            if (filled_ == header_.size()) {
              opcode_ = wire::read32(header_, 8);
              length_ = wire::read32(header_, 12);
              if (wire::read32(header_, 0) != kProtocolMagic ||
                  wire::read32(header_, 4) != 1 || length_ > payload_.size()) {
                engine_.abort(Status::parse_error);
                ns = State::failed;
              } else {
                filled_ = 0;
                ns = State::payload;
              }
            }
            break;
          }
          case State::payload: {
            count = std::min<std::size_t>(input.size(), length_ - filled_);
            std::copy_n(input.begin(), count, payload_.begin() + filled_);
            filled_ += count;
            if (filled_ == length_) {
              Reply(Dispatch());
              ns = State::reply;
            }
            break;
          }
          case State::reply: {
            if (sent_) {
              sent_ = false;
              filled_ = 0;
              ns = State::header;
            }
            break;
          }
          case State::failed:
            break;
        }
        break;
      }
    }
  }


}  // namespace daveos::update
