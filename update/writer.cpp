#include "writer.h"

#include <algorithm>

namespace daveos::update {


  Status Writer::begin(std::uint32_t slot, std::uint32_t offset,
                       std::span<const std::byte> data) {
    if (status_ == Status::busy) {
      return Status::busy;
    }
    if (!layout_.valid() || !flash_.valid() || slot >= 2 || data.empty() ||
        offset % layout_.write_size || offset >= layout_.slot_size ||
        data.size() > layout_.slot_size - offset) {
      return Status::invalid_argument;
    }
    if (flash_.poll(flash_.context) == Status::busy) {
      return Status::busy;
    }
    base_ = layout_.slots[slot];
    offset_ = offset;
    if (!offset) {
      erased_until_ = 0;
    }
    written_ = 0;
    data_ = data;
    requested_ = true;
    cancelled_ = false;
    status_ = Status::busy;
    return Status::ok;
  }

  void Writer::tick() { (void)machine_.tick(*this); }

  void Writer::Step(State cs, State& ns) {
    switch (cs) {
      case State::idle:
      case State::done:
      case State::failed: {
        if (requested_) {
          requested_ = false;
          ns = State::prepare;
        }
        break;
      }
      case State::prepare: {
        if (cancelled_) {
          status_ = Status::rejected;
          ns = State::done;
        } else if (written_ == data_.size()) {
          status_ = Status::ok;
          ns = State::done;
        } else {
          const auto position = offset_ + static_cast<std::uint32_t>(written_);
          Status result;
          if (position >= erased_until_) {
            auto sector = position - position % layout_.sector_size;
            result = flash_.erase(flash_.context, base_ + sector);
            erased_until_ = sector + layout_.sector_size;
            ns = State::erase_wait;
          } else {
            word_.fill(std::byte{0xff});
            auto count = std::min(word_.size(), data_.size() - written_);
            std::copy_n(data_.begin() + written_, count, word_.begin());
            result = flash_.program(flash_.context, base_ + position, word_);
            ns = State::program_wait;
          }
          operation_started_ = flash_.now(flash_.context);
          if (result != Status::ok) {
            status_ = result == Status::busy ? Status::io_error : result;
            ns = State::failed;
          }
        }
        break;
      }
      case State::erase_wait:
      case State::program_wait: {
        auto result = flash_.poll(flash_.context);
        if (result == Status::busy &&
            flash_.now(flash_.context) - operation_started_ >= 1000000) {
          result = Status::timeout;
        }
        if (result != Status::busy) {
          if (result != Status::ok) {
            status_ = result;
            ns = State::failed;
          } else if (cancelled_) {
            status_ = Status::rejected;
            ns = State::done;
          } else {
            if (cs == State::program_wait) {
              written_ += std::min(word_.size(), data_.size() - written_);
            }
            ns = State::prepare;
          }
        }
        break;
      }
    }
  }


}  // namespace daveos::update
