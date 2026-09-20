#include "engine.h"

#include <algorithm>
#include <limits>

namespace daveos::update {


  bool Engine::active() const {
    return requested_ ||
           (state() != State::disabled && state() != State::idle &&
            state() != State::done && state() != State::failed);
  }

  bool Engine::ready() const {
    return enabled_ && !abort_requested_ && state() == State::receiving &&
           !rx_size_ && received_ < total() &&
           reader_.state() != PackageReader::State::ready;
  }

  Status Engine::begin(std::span<const std::byte> bytes) {
    if (!enabled_) {
      return Status::not_running;
    }
    if (active()) {
      return Status::busy;
    }
    if (running_slot_ >= 2) {
      return Status::unsupported;
    }
    auto result = decode_header(bytes, layout_, 1 - running_slot_, header_);
    if (result != Status::ok) {
      return result;
    }
    result = journal_.load(snapshot_);
    if (result != Status::ok) {
      return result;
    }
    if (snapshot_.images[running_slot_].state != boot::ImageState::confirmed) {
      return Status::not_confirmed;
    }
    if (snapshot_.counter == std::numeric_limits<std::uint64_t>::max()) {
      return Status::counter_exhausted;
    }
    if (flash_.poll(flash_.context) == Status::busy) {
      return Status::busy;
    }
    requested_ = true;
    abort_requested_ = false;
    received_ = verify_offset_ = verify_crc_ = 0;
    rx_size_ = rx_at_ = 0;
    status_ = Status::busy;
    return Status::ok;
  }

  Status Engine::chunk(std::uint32_t offset, std::span<const std::byte> data,
                       std::uint32_t crc) {
    if (!ready()) {
      return Status::busy;
    }
    if (data.empty() || data.size() > rx_.size() || offset != received_ ||
        data.size() > total() - received_) {
      return Status::invalid_argument;
    }
    if (crc_.calculate(data) != crc) {
      return Status::checksum_error;
    }
    std::copy(data.begin(), data.end(), rx_.begin());
    rx_size_ = data.size();
    rx_at_ = 0;
    received_ += data.size();
    return Status::ok;
  }

  void Engine::abort(Status reason) {
    if (active()) {
      abort_requested_ = true;
      abort_reason_ = reason;
    }
  }

  void Engine::Fail(Status status) {
    abort_reason_ = status;
    abort_requested_ = true;
  }

  void Engine::tick() { (void)machine_.tick(*this); }

  void Engine::Step(State cs, State& ns) {
    switch (cs) {
      case State::disabled:
      case State::idle:
      case State::done:
      case State::failed: {
        if (requested_) {
          requested_ = false;
          snapshot_.images[1 - running_slot_].state =
              boot::ImageState::incomplete;
          auto result = journal_.begin(snapshot_);
          if (result == Status::ok) {
            ns = State::invalidating;
          } else {
            status_ = result;
            ns = State::failed;
          }
        } else if (!enabled_) {
          ns = State::disabled;
        } else if (cs == State::disabled) {
          status_ = Status::ok;
          ns = State::idle;
        }
        break;
      }
      case State::invalidating: {
        journal_.tick();
        if (!enabled_ || abort_requested_) {
          ns = State::aborting;
        } else if (journal_.status() != Status::busy) {
          if (journal_.status() != Status::ok) {
            status_ = journal_.status();
            ns = State::failed;
          } else {
            reader_.start(header_, 1 - running_slot_);
            reader_.tick();
            waiting_since_ = flash_.now(flash_.context);
            ns = State::receiving;
          }
        }
        break;
      }
      case State::receiving: {
        if (!enabled_ || abort_requested_) {
          ns = State::aborting;
          break;
        }
        rx_at_ +=
            reader_.tick(std::span(rx_).subspan(rx_at_, rx_size_ - rx_at_));
        if (rx_size_ && rx_at_ == rx_size_) {
          rx_size_ = rx_at_ = 0;
          waiting_since_ = flash_.now(flash_.context);
        }
        if (reader_.state() == PackageReader::State::failed) {
          Fail(reader_.status());
          ns = State::aborting;
        } else if (reader_.state() == PackageReader::State::ready) {
          auto result = writer_.begin(1 - running_slot_, reader_.offset(),
                                      reader_.block());
          if (result == Status::ok) {
            ns = State::writing;
          } else {
            Fail(result);
            ns = State::aborting;
          }
        } else if (reader_.state() == PackageReader::State::done) {
          if (rx_size_ || received_ != total()) {
            Fail(Status::parse_error);
            ns = State::aborting;
          } else {
            ns = State::verifying;
          }
        } else if (!rx_size_ && received_ == total()) {
          Fail(Status::parse_error);
          ns = State::aborting;
        } else if (!rx_size_ &&
                   flash_.now(flash_.context) - waiting_since_ >= inactivity_) {
          Fail(Status::timeout);
          ns = State::aborting;
        }
        break;
      }
      case State::writing: {
        if (!enabled_ || abort_requested_) {
          writer_.cancel();
          ns = State::aborting;
        } else {
          writer_.tick();
          if (writer_.status() != Status::busy) {
            if (writer_.status() != Status::ok) {
              Fail(writer_.status());
              ns = State::aborting;
            } else {
              reader_.release();
              waiting_since_ = flash_.now(flash_.context);
              ns = State::receiving;
            }
          }
        }
        break;
      }
      case State::verifying: {
        if (!enabled_ || abort_requested_) {
          ns = State::aborting;
          break;
        }
        auto bytes =
            std::span(verification_)
                .first(std::min<std::size_t>(
                    verification_.size(), header_.image_size - verify_offset_));
        auto result = flash_.read(
            flash_.context, layout_.slots[1 - running_slot_] + verify_offset_,
            bytes);
        if (result != Status::ok) {
          Fail(result);
          ns = State::aborting;
          break;
        }
        verify_crc_ = crc_.update(verify_crc_, bytes);
        verify_offset_ += bytes.size();
        if (verify_offset_ == header_.image_size) {
          if (verify_crc_ != header_.crc[1 - running_slot_]) {
            Fail(Status::checksum_error);
            ns = State::aborting;
          } else {
            result = journal_.load(snapshot_);
            if (result == Status::ok &&
                snapshot_.counter ==
                    std::numeric_limits<std::uint64_t>::max()) {
              result = Status::counter_exhausted;
            }
            if (result == Status::ok) {
              ++snapshot_.counter;
              snapshot_.images[1 - running_slot_] = {
                  snapshot_.counter, header_.image_size,
                  verify_crc_,       boot::ImageState::pending,
                  header_.product,   header_.revision,
                  header_.version};
              result = journal_.begin(snapshot_);
            }
            if (result == Status::ok) {
              ns = State::committing;
            } else {
              Fail(result);
              ns = State::aborting;
            }
          }
        }
        break;
      }
      case State::committing: {
        journal_.tick();
        if (!enabled_ || abort_requested_) {
          ns = State::aborting;
        } else if (journal_.status() != Status::busy) {
          status_ = journal_.status();
          if (status_ == Status::ok) {
            ns = State::done;
          } else {
            Fail(status_);
            ns = State::aborting;
          }
        }
        break;
      }
      case State::aborting: {
        if (writer_.status() == Status::busy) {
          writer_.cancel();
          writer_.tick();
          break;
        }
        if (journal_.status() == Status::busy) {
          journal_.tick();
          break;
        }
        if (flash_.poll(flash_.context) == Status::busy) {
          break;
        }
        // A disable may arrive while the final commit marker is in flight.
        // Finish that operation, then invalidate the newly committed candidate.
        auto result = journal_.load(snapshot_);
        if (result == Status::ok && snapshot_.images[1 - running_slot_].state ==
                                        boot::ImageState::pending) {
          snapshot_.images[1 - running_slot_].state =
              boot::ImageState::incomplete;
          result = journal_.begin(snapshot_);
          if (result == Status::ok) {
            break;
          }
        }
        status_ = result == Status::ok ? abort_reason_ : result;
        abort_requested_ = false;
        rx_size_ = rx_at_ = 0;
        ns = State::failed;
        break;
      }
    }
  }


}  // namespace daveos::update
