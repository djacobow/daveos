#include "file.h"

#include <algorithm>

namespace daveos::update {


  Status FileUpdate::prepare(std::string_view path) {
    if (busy_) {
      return Status::busy;
    }
    if (!source_) {
      return Status::unsupported;
    }
    if (path.empty() || path.size() > kPathCapacity ||
        path.find('\0') != path.npos || path.find(':') != path.npos) {
      return Status::invalid_argument;
    }
    auto result = engine_.reserve(this);
    if (result != Status::ok) {
      return result;
    }
    result = source_.reserve(source_.context);
    if (result != Status::ok) {
      engine_.release(this);
      return result;
    }
    std::copy(path.begin(), path.end(), path_.begin());
    path_[path.size()] = '\0';
    header_ = {};
    filled_ = at_ = received_ = crc_ = installed_ = 0;
    cancel_requested_ = install_requested_ = installing_ = false;
    status_ = Status::busy;
    requested_ = busy_ = true;
    return Status::ok;
  }

  Status FileUpdate::install() {
    if (state() != State::prepared || !busy_ || cancel_requested_) {
      return Status::not_running;
    }
    if (install_requested_) {
      return Status::busy;
    }
    install_requested_ = true;
    return Status::ok;
  }

  bool FileUpdate::Read(std::span<std::byte> bytes, std::uint32_t& count) {
    count = 0;
    status_ = source_.read(source_.context, bytes, count);
    if (status_ == Status::ok && (count == 0 || count > bytes.size())) {
      status_ = Status::parse_error;
    }
    return status_ == Status::ok;
  }

  const char* FileUpdate::phase() const {
    switch (state()) {
      case State::idle:
        return "idle";
      case State::prepared:
        return "prepared";
      case State::rewind:
      case State::checking:
      case State::starting:
      case State::installing:
        return "installing";
      case State::cleanup:
        return "cleanup";
      case State::done:
        return "done";
      case State::failed:
        return "failed";
      default:
        return "validating";
    }
  }

  void FileUpdate::Step(State cs, State& ns) {
    switch (cs) {
      case State::idle:
      case State::opening:
      case State::header:
      case State::validating:
      case State::eof:
      case State::prepared:
      case State::rewind:
      case State::checking:
      case State::starting:
      case State::installing:
      case State::cleanup:
      case State::done:
      case State::failed: {
        if (cancel_requested_ && cs != State::cleanup) {
          cancel_requested_ = false;
          requested_ = install_requested_ = false;
          status_ = Status::rejected;
          ns = State::cleanup;
          break;
        }
        switch (cs) {
          case State::idle:
          case State::done:
          case State::failed:
            if (requested_) {
              requested_ = false;
              ns = State::opening;
            }
            break;
          case State::opening:
            status_ = source_.open(source_.context, path_.data());
            opened_ = status_ == Status::ok;
            ns = opened_ ? State::header : State::cleanup;
            break;
          case State::header: {
            std::uint32_t count;
            if (!Read(std::span(header_bytes_).subspan(filled_), count)) {
              ns = State::cleanup;
              break;
            }
            filled_ += count;
            if (filled_ == kHeaderSize) {
              status_ = engine_.inspect(header_bytes_, header_);
              if (status_ != Status::ok) {
                ns = State::cleanup;
                break;
              }
              reader_.start(header_, destination());
              reader_.tick();
              filled_ = at_ = 0;
              ns = State::validating;
            }
            break;
          }
          case State::validating:
            if (reader_.state() == PackageReader::State::failed) {
              status_ = reader_.status();
              ns = State::cleanup;
            } else if (reader_.state() == PackageReader::State::ready) {
              crc_ = util::crc32::update(crc_, reader_.block());
              reader_.release();
              reader_.tick();
            } else if (reader_.state() == PackageReader::State::done) {
              if (at_ != filled_ || received_ != total()) {
                status_ = Status::parse_error;
                ns = State::cleanup;
              } else if (crc_ != header_.crc[destination()]) {
                status_ = Status::checksum_error;
                ns = State::cleanup;
              } else {
                ns = State::eof;
              }
            } else if (at_ < filled_) {
              at_ +=
                  reader_.tick(std::span(buffer_).subspan(at_, filled_ - at_));
            } else if (received_ < total()) {
              std::uint32_t count;
              if (!Read(std::span(buffer_).first(std::min<std::size_t>(
                            buffer_.size(), total() - received_)),
                        count)) {
                ns = State::cleanup;
                break;
              }
              received_ += count;
              filled_ = count;
              at_ = 0;
            } else {
              status_ = Status::parse_error;
              ns = State::cleanup;
            }
            break;
          case State::eof: {
            std::uint32_t count = 0;
            status_ = source_.read(source_.context, std::span(buffer_).first(1),
                                   count);
            if (status_ == Status::ok && count) {
              status_ = Status::parse_error;
            }
            ns = status_ == Status::ok ? State::prepared : State::cleanup;
            break;
          }
          case State::prepared:
            if (install_requested_) {
              install_requested_ = false;
              filled_ = 0;
              ns = State::rewind;
            }
            break;
          case State::rewind:
            status_ = source_.seek(source_.context, 0);
            ns = status_ == Status::ok ? State::checking : State::cleanup;
            break;
          case State::checking: {
            std::uint32_t count;
            if (!Read(
                    std::span(buffer_).subspan(filled_, kHeaderSize - filled_),
                    count)) {
              ns = State::cleanup;
              break;
            }
            filled_ += count;
            if (filled_ == kHeaderSize) {
              if (!std::equal(header_bytes_.begin(), header_bytes_.end(),
                              buffer_.begin())) {
                status_ = Status::checksum_error;
                ns = State::cleanup;
              } else {
                ns = State::starting;
              }
            }
            break;
          }
          case State::starting:
            status_ = engine_.begin(header_bytes_, this);
            installing_ = status_ == Status::ok;
            ns = installing_ ? State::installing : State::cleanup;
            break;
          case State::installing:
            installed_ = engine_.next_offset();
            if (!engine_.active()) {
              status_ = engine_.status();
              ns = State::cleanup;
            } else if (engine_.ready()) {
              std::uint32_t count;
              if (!Read(std::span(buffer_).first(std::min<std::size_t>(
                            buffer_.size(),
                            engine_.total() - engine_.next_offset())),
                        count)) {
                ns = State::cleanup;
                break;
              }
              if (count == engine_.total() - engine_.next_offset()) {
                // Finish all fallible file I/O before submitting the final
                // payload. Keep the volume reservation through flash commit.
                std::byte extra{};
                std::uint32_t trailing = 0;
                status_ = source_.read(source_.context, {&extra, 1}, trailing);
                if (status_ == Status::ok && trailing) {
                  status_ = Status::parse_error;
                }
                if (status_ != Status::ok) {
                  ns = State::cleanup;
                  break;
                }
                status_ = source_.close(source_.context);
                opened_ = false;
                if (status_ != Status::ok) {
                  ns = State::cleanup;
                  break;
                }
              }
              const auto bytes = std::span(buffer_).first(count);
              status_ = engine_.chunk(engine_.next_offset(), bytes,
                                      util::crc32::calculate(bytes));
              if (status_ != Status::ok) {
                ns = State::cleanup;
              }
            }
            break;
          case State::cleanup:
            if (engine_.active()) {
              engine_.abort(status_);
              break;
            }
            if (opened_) {
              const auto result = source_.close(source_.context);
              opened_ = false;
              if (status_ == Status::ok) {
                status_ = result;
              }
            }
            source_.release(source_.context);
            engine_.release(this);
            cancel_requested_ = false;
            busy_ = false;
            ns = status_ == Status::ok ? State::done : State::failed;
            break;
        }
        break;
      }
    }
  }


}  // namespace daveos::update
