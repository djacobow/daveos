#include "journal.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "util/crc32.h"
#include "util/wire.h"

namespace daveos::boot {


  namespace wire = util::wire;

  namespace {
    constexpr std::uint32_t kMagic = 0x4a534f44;
    constexpr std::uint32_t kCommit = 0x454e4f44;
    constexpr std::size_t kCrcOffset = 236;
    constexpr std::size_t kCommitOffset = 240;

    bool Erased(const Flash& flash, std::uint32_t address) {
      Record record;
      const auto blank = [](auto bytes) {
        return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) {
          return value == std::byte{0xff};
        });
      };
      auto header = std::span(record).first(16);
      if (flash.read(flash.context, address, header) != Status::ok ||
          !blank(header)) {
        return false;
      }
      auto rest = std::span(record).subspan(16);
      return flash.read(flash.context, address + 16, rest) == Status::ok &&
             blank(rest);
    }
  }  // namespace

  bool Layout::valid() const {
    if (!slot_size || !sector_size || !operation_timeout ||
        sector_size % kRecordSize || (write_size != 16 && write_size != 32) ||
        slot_size % sector_size || !product || !revision) {
      return false;
    }
    std::array<std::pair<std::uint64_t, std::uint64_t>, 4> regions{};
    for (std::size_t i = 0; i < 2; ++i) {
      if (slots[i] % sector_size || metadata[i] % sector_size) {
        return false;
      }
      regions[i] = {slots[i], std::uint64_t(slots[i]) + slot_size};
      regions[i + 2] = {metadata[i], std::uint64_t(metadata[i]) + sector_size};
    }
    for (std::size_t i = 0; i < regions.size(); ++i) {
      if (regions[i].second > UINT64_C(0x100000000)) {
        return false;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (regions[i].first < regions[j].second &&
            regions[j].first < regions[i].second) {
          return false;
        }
      }
    }
    return true;
  }

  Record encode(const Snapshot& snapshot, std::uint32_t write_size) {
    const auto crc_offset = write_size == 32 ? 220u : kCrcOffset;
    const auto commit_offset = write_size == 32 ? 224u : kCommitOffset;
    Record bytes{};
    wire::write32(bytes, 0, kMagic);
    wire::write32(bytes, 4, write_size == 32 ? 2 : 1);
    wire::write64(bytes, 8, snapshot.sequence);
    wire::write64(bytes, 16, snapshot.counter);
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& image = snapshot.images[i];
      auto at = 24 + i * 96;
      wire::write64(bytes, at, image.installation);
      wire::write32(bytes, at + 8, image.size);
      wire::write32(bytes, at + 12, image.crc);
      wire::write32(bytes, at + 16, static_cast<std::uint32_t>(image.state));
      wire::write32(bytes, at + 20, image.product);
      wire::write32(bytes, at + 24, image.revision);
      wire::write32(bytes, at + 28, image.version.major);
      wire::write32(bytes, at + 32, image.version.minor);
      wire::write32(bytes, at + 36, image.version.build);
      std::memcpy(bytes.data() + at + 40, image.version.commit, 41);
      bytes[at + 81] = std::byte(image.version.dirty);
    }
    wire::write32(bytes, crc_offset,
                  util::crc32::calculate(std::span(bytes).first(crc_offset)));
    wire::write32(bytes, commit_offset, kCommit);
    wire::write64(bytes, commit_offset + 4, snapshot.sequence);
    wire::write32(bytes, commit_offset + 12, ~kCommit);
    return bytes;
  }

  bool decode(const Record& bytes, Snapshot& snapshot) {
    const auto version = wire::read32(bytes, 4);
    const auto crc_offset = version == 2 ? 220u : kCrcOffset;
    const auto commit_offset = version == 2 ? 224u : kCommitOffset;
    if (wire::read32(bytes, 0) != kMagic || (version != 1 && version != 2) ||
        wire::read32(bytes, commit_offset) != kCommit ||
        wire::read32(bytes, commit_offset + 12) != ~kCommit ||
        wire::read64(bytes, commit_offset + 4) != wire::read64(bytes, 8) ||
        wire::read32(bytes, crc_offset) !=
            util::crc32::calculate(std::span(bytes).first(crc_offset))) {
      return false;
    }
    if (version == 2 &&
        !std::all_of(bytes.begin() + 240, bytes.end(),
                     [](std::byte b) { return b == std::byte{0}; })) {
      return false;
    }
    Snapshot result{};
    result.sequence = wire::read64(bytes, 8);
    result.counter = wire::read64(bytes, 16);
    if (!result.sequence) {
      return false;
    }
    for (std::size_t i = 0; i < 2; ++i) {
      auto& image = result.images[i];
      auto at = 24 + i * 96;
      image.installation = wire::read64(bytes, at);
      image.size = wire::read32(bytes, at + 8);
      image.crc = wire::read32(bytes, at + 12);
      auto state = wire::read32(bytes, at + 16);
      if (state > static_cast<std::uint32_t>(ImageState::rejected) ||
          image.installation > result.counter) {
        return false;
      }
      image.state = static_cast<ImageState>(state);
      image.product = wire::read32(bytes, at + 20);
      image.revision = wire::read32(bytes, at + 24);
      image.version.major = wire::read32(bytes, at + 28);
      image.version.minor = wire::read32(bytes, at + 32);
      image.version.build = wire::read32(bytes, at + 36);
      std::memcpy(image.version.commit, bytes.data() + at + 40, 41);
      image.version.commit[40] = '\0';
      image.version.dirty = bytes[at + 81] != std::byte{0};
    }
    snapshot = result;
    return true;
  }

  Status Journal::load(Snapshot& snapshot) {
    if (!flash_.valid() || !layout_.valid()) {
      return Status::invalid_argument;
    }
    if (flash_.poll(flash_.context) == Status::busy) {
      return Status::busy;
    }
    Snapshot best{};
    Record best_record{};
    bool found = false;
    bool read_failed = false;
    for (auto sector : layout_.metadata) {
      for (std::uint32_t remaining = layout_.sector_size; remaining;
           remaining -= kRecordSize) {
        const auto offset = remaining - kRecordSize;
        Record bytes;
        // Most entries are erased or invalid. Read their identifying header
        // before copying/decoding a whole record; large H7 sectors must not
        // turn a foreground metadata query into a long scheduler stall.
        if (flash_.read(flash_.context, sector + offset,
                        std::span(bytes).first(16)) != Status::ok) {
          read_failed = true;
          continue;
        }
        if (wire::read32(bytes, 0) != kMagic ||
            (wire::read32(bytes, 4) != 1 && wire::read32(bytes, 4) != 2)) {
          continue;
        }
        if (found && wire::read64(bytes, 8) < best.sequence) {
          continue;
        }
        if (flash_.read(flash_.context, sector + offset + 16,
                        std::span(bytes).subspan(16)) != Status::ok) {
          read_failed = true;
          continue;
        }
        Snapshot candidate;
        if (!decode(bytes, candidate)) {
          continue;
        }
        if (found && candidate.sequence == best.sequence &&
            bytes != best_record) {
          return Status::checksum_error;
        }
        if (!found || candidate.sequence > best.sequence) {
          best = candidate;
          best_record = bytes;
          latest_ = sector + offset;
          found = true;
        }
      }
    }
    if (!found) {
      return read_failed ? Status::io_error : Status::not_found;
    }
    snapshot = best;
    return Status::ok;
  }

  Status Journal::begin(const Snapshot& snapshot, bool initialize) {
    if (status_ == Status::busy) {
      return Status::busy;
    }
    Snapshot current;
    auto loaded = load(current);
    if (loaded != Status::ok && !(initialize && loaded == Status::not_found)) {
      return loaded;
    }
    if (flash_.poll(flash_.context) == Status::busy) {
      return Status::busy;
    }
    if (current.sequence == std::numeric_limits<std::uint64_t>::max() ||
        snapshot.counter < current.counter) {
      return Status::counter_exhausted;
    }
    auto next = snapshot;
    next.sequence = current.sequence + 1;
    record_ = encode(next, layout_.write_size);
    checkpoint_ = false;
    erase_only_ = false;
    Snapshot check;
    if (!decode(record_, check)) {
      return Status::invalid_argument;
    }
    erase_needed_ = true;
    target_ = layout_.metadata[0];
    if (loaded == Status::ok) {
      const auto bank =
          latest_ >= layout_.metadata[1] &&
                  latest_ - layout_.metadata[1] < layout_.sector_size
              ? 1u
              : 0u;
      for (auto at = latest_ + kRecordSize;
           at < std::uint64_t(layout_.metadata[bank]) + layout_.sector_size;
           at += kRecordSize) {
        if (Erased(flash_, at)) {
          target_ = at;
          erase_needed_ = false;
          break;
        }
      }
      if (erase_needed_) {
        target_ = layout_.metadata[1 - bank];
      }
    }
    const auto executing =
        flash_.executing_bank ? flash_.executing_bank(flash_.context) : 2u;
    if (executing < 2) {
      // Runtime appends/erases belong to the inactive bank. Checkpoint the
      // previous snapshot in the executing bank before reclaiming its only
      // durable copy; never erase the executing bank to obtain space.
      const auto inactive = 1 - executing;
      auto free_record = [&](std::uint32_t bank) {
        for (auto at = layout_.metadata[bank];
             at < std::uint64_t(layout_.metadata[bank]) + layout_.sector_size;
             at += kRecordSize) {
          if (Erased(flash_, at)) {
            return at;
          }
        }
        return UINT32_MAX;
      };
      target_ = free_record(inactive);
      erase_needed_ = target_ == UINT32_MAX;
      if (erase_needed_) {
        target_ = layout_.metadata[inactive];
        if (loaded == Status::ok && latest_ >= layout_.metadata[inactive] &&
            latest_ < std::uint64_t(layout_.metadata[inactive]) +
                          layout_.sector_size) {
          const auto checkpoint = free_record(executing);
          if (checkpoint == UINT32_MAX) {
            return Status::full;
          }
          pending_record_ = record_;
          pending_target_ = target_;
          // Preserve the exact encoding for duplicate-sequence validation.
          if (flash_.read(flash_.context, latest_, record_) != Status::ok) {
            return Status::io_error;
          }
          target_ = checkpoint;
          checkpoint_ = true;
          erase_needed_ = false;
        }
      }
    }
    offset_ = 0;
    timed_out_ = false;
    requested_ = true;
    status_ = Status::busy;
    return Status::ok;
  }

  void Journal::tick() {
    if (requested_) {
      operation_started_ = flash_.now(flash_.context);
    } else if (status_ == Status::busy &&
               flash_.now(flash_.context) - operation_started_ >=
                   layout_.operation_timeout) {
      timed_out_ = true;
    }
    (void)machine_.tick(*this);
  }

  void Journal::Step(State cs, State& ns) {
    switch (cs) {
      case State::idle:
      case State::done:
      case State::failed: {
        if (requested_) {
          requested_ = false;
          ns = erase_needed_ ? State::erase : State::write;
        }
        break;
      }
      case State::erase: {
        if (timed_out_) {
          status_ = Status::timeout;
          ns = State::failed;
          break;
        }
        auto result = flash_.erase(flash_.context, target_);
        if (result != Status::ok) {
          status_ = result == Status::busy ? Status::io_error : result;
          ns = State::failed;
        } else {
          ns = State::erase_wait;
        }
        break;
      }
      case State::erase_wait:
      case State::write_wait: {
        auto result = flash_.poll(flash_.context);
        if (result == Status::busy && !timed_out_) {
          break;
        }
        if (result != Status::ok || timed_out_) {
          status_ = timed_out_ ? Status::timeout : result;
          ns = State::failed;
        } else if (cs == State::erase_wait) {
          if (erase_only_) {
            status_ = Status::ok;
            ns = State::done;
          } else {
            ns = State::write;
          }
        } else {
          offset_ += layout_.write_size;
          ns = offset_ == record_.size() ? State::verify : State::write;
        }
        break;
      }
      case State::write: {
        if (timed_out_) {
          status_ = Status::timeout;
          ns = State::failed;
          break;
        }
        auto result = flash_.program(
            flash_.context, target_ + offset_,
            std::span(record_).subspan(offset_, layout_.write_size));
        if (result != Status::ok) {
          status_ = result == Status::busy ? Status::io_error : result;
          ns = State::failed;
        } else {
          ns = State::write_wait;
        }
        break;
      }
      case State::verify: {
        Record readback;
        status_ = flash_.read(flash_.context, target_, readback);
        if (status_ == Status::ok && readback != record_) {
          status_ = Status::checksum_error;
        }
        if (status_ == Status::ok && checkpoint_) {
          record_ = pending_record_;
          target_ = pending_target_;
          offset_ = 0;
          checkpoint_ = false;
          status_ = Status::busy;
          ns = State::erase;
        } else {
          ns = status_ == Status::ok ? State::done : State::failed;
        }
        break;
      }
    }
  }

  Status Journal::prepare_runtime(std::uint32_t bank, core::Time timeout) {
    if (bank >= 2 ||
        (flash_.executing_bank && flash_.executing_bank(flash_.context) < 2)) {
      return Status::unsupported;
    }
    if (status_ == Status::busy) {
      return Status::busy;
    }
    Snapshot snapshot;
    auto result = load(snapshot);
    if (result != Status::ok) {
      return result;
    }
    const auto run = [&] {
      offset_ = 0;
      timed_out_ = false;
      checkpoint_ = false;
      requested_ = true;
      status_ = Status::busy;
      const auto start = flash_.now(flash_.context);
      while (status_ == Status::busy) {
        timed_out_ = flash_.now(flash_.context) - start >= timeout;
        tick();
      }
      return status_;
    };
    if (latest_ >= layout_.metadata[bank] &&
        latest_ < std::uint64_t(layout_.metadata[bank]) + layout_.sector_size) {
      if (flash_.read(flash_.context, latest_, record_) != Status::ok) {
        return Status::io_error;
      }
      target_ = layout_.metadata[1 - bank];
      erase_needed_ = true;
      for (auto at = target_;
           at < std::uint64_t(layout_.metadata[1 - bank]) + layout_.sector_size;
           at += kRecordSize) {
        if (Erased(flash_, at)) {
          target_ = at;
          erase_needed_ = false;
          break;
        }
      }
      erase_only_ = false;
      result = run();
      if (result != Status::ok) {
        return result;
      }
    }
    bool blank = true;
    for (auto at = layout_.metadata[bank];
         blank &&
         at < std::uint64_t(layout_.metadata[bank]) + layout_.sector_size;
         at += kRecordSize) {
      blank = Erased(flash_, at);
    }
    if (blank) {
      return Status::ok;
    }
    target_ = layout_.metadata[bank];
    erase_needed_ = true;
    erase_only_ = true;
    return run();
  }

  Status Journal::commit(const Snapshot& snapshot, core::Time timeout,
                         bool initialize) {
    if (!timeout) {
      timeout = layout_.operation_timeout;
    }
    auto result = begin(snapshot, initialize);
    if (result != Status::ok) {
      return result;
    }
    const auto start = flash_.now(flash_.context);
    while (status_ == Status::busy) {
      timed_out_ = flash_.now(flash_.context) - start >= timeout;
      tick();
    }
    return status_;
  }


}  // namespace daveos::boot
