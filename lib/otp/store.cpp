#include "store.h"

#include <limits>

namespace daveos::otp {


  Store::Kind Store::Classify(const Block& block) const {
    if (block.storage == Storage::unreadable) {
      return Kind::unreadable;
    }
    if (block.storage == Storage::unused && !block.locked) {
      return Kind::unused;
    }
    if (!block.record.valid(crc_)) {
      return Kind::invalid;
    }
    if (block.record.type() != std::uint16_t(Type::serial_number)) {
      return Kind::unknown;
    }
    return valid_serial(block.record.payload()) ? Kind::valid : Kind::invalid;
  }

  Status Store::Error(Status status, Phase phase,
                      std::optional<std::uint32_t> block) {
    if (diagnostics_.errors != std::numeric_limits<std::uint32_t>::max()) {
      ++diagnostics_.errors;
    }
    diagnostics_.error = status;
    diagnostics_.phase = phase;
    diagnostics_.error_block = block;
    return status;
  }

  Status Store::init() {
    if (initialized_) {
      return ready_ ? Status::already_initialized
                    : Status::initialization_failed;
    }
    initialized_ = true;
    if (!driver_.valid() || driver_.blocks != kBlockCount ||
        driver_.block_size != kRecordSize) {
      return Error(Status::incompatible, Phase::scan);
    }
    auto status = driver_.init(driver_.context);
    if (status != Status::ok) {
      return Error(status, Phase::scan);
    }
    for (std::uint32_t i = 0; i < kBlockCount; ++i) {
      Block block;
      status = driver_.inspect(driver_.context, i, block);
      if (status != Status::ok) {
        return Error(status, Phase::scan, i);
      }
      auto kind = Classify(block);
      kinds_[i] = kind;
      if (kind != Kind::unused) {
        next_ = i + 1;
      }
      if (kind == Kind::unreadable) {
        Error(Status::io_error, Phase::scan, i);
      }
      if (kind == Kind::valid) {
        serial_ = block.record;
        serial_block_ = i;
        locked_ = block.locked;
      }
    }
    ready_ = true;
    return Status::ok;
  }

  std::optional<std::string_view> Store::serial() const {
    if (!ready_ || !serial_block_) {
      return {};
    }
    return serial_.payload();
  }

  Status Store::Lock(std::uint32_t index) {
    auto status = driver_.lock(driver_.context, index);
    Block block;
    const auto inspected = driver_.inspect(driver_.context, index, block);
    locked_ = inspected == Status::ok && block.locked;
    if (status != Status::ok) {
      return Error(status, Phase::lock, index);
    }
    if (!locked_) {
      return Error(inspected == Status::ok ? Status::io_error : inspected,
                   Phase::lock, index);
    }
    return Status::ok;
  }

  Status Store::set_serial(std::string_view value) {
    if (!ready_) {
      return Status::not_running;
    }
    if (!valid_serial(value)) {
      return Status::invalid_argument;
    }
    if (serial_block_ && serial_.payload() == value) {
      return locked_ ? Status::ok : Lock(*serial_block_);
    }
    if (next_ == kBlockCount) {
      return Status::full;
    }
    // Stage before replacing the cache: value may borrow serial_'s bytes.
    const auto proposed = serial_record(value, crc_);
    const auto index = next_;
    const auto result = driver_.program(driver_.context, index, proposed);
    if (result.attempted || result.status == Status::ok) {
      ++next_;
      kinds_[index] = Kind::invalid;
    }
    if (result.status != Status::ok) {
      return Error(result.status, Phase::program, index);
    }
    Block block;
    auto status = driver_.inspect(driver_.context, index, block);
    if (status != Status::ok || block.storage == Storage::unreadable) {
      kinds_[index] = Kind::unreadable;
      return Error(status == Status::ok ? Status::io_error : status,
                   Phase::verify, index);
    }
    if (Classify(block) != Kind::valid ||
        block.record.bytes != proposed.bytes) {
      return Error(Status::checksum_error, Phase::verify, index);
    }
    kinds_[index] = Kind::valid;
    serial_ = block.record;
    serial_block_ = index;
    locked_ = block.locked;
    return Lock(index);
  }

  Snapshot Store::snapshot() const {
    auto result = diagnostics_;
    result.backend = driver_.name;
    result.ready = ready_;
    result.remaining = ready_ ? kBlockCount - next_ : 0;
    result.serial_block = ready_ ? serial_block_ : std::nullopt;
    result.serial_locked = ready_ && locked_;
    for (auto kind : kinds_) {
      result.consumed += kind != Kind::unused;
      result.invalid += kind == Kind::invalid;
      result.unknown += kind == Kind::unknown;
      result.unreadable += kind == Kind::unreadable;
    }
    result.gaps = next_ - result.consumed;
    return result;
  }


}  // namespace daveos::otp
