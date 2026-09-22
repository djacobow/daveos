#include "transfer.h"

#include <algorithm>

#include "util/crc32.h"
#include "util/wire.h"

namespace daveos::storage {


  TransferStatus FileTransfer::Map(FRESULT result) {
    switch (result) {
      case FR_OK:
        return TransferStatus::ok;
      case FR_DENIED:
        return TransferStatus::denied;
      case FR_LOCKED:
        return TransferStatus::busy;
      case FR_NOT_READY:
        return TransferStatus::not_ready;
      case FR_WRITE_PROTECTED:
        return TransferStatus::read_only;
      case FR_EXIST:
        return TransferStatus::exists;
      case FR_NO_FILE:
      case FR_NO_PATH:
        return TransferStatus::not_found;
      case FR_INVALID_NAME:
      case FR_INVALID_PARAMETER:
        return TransferStatus::invalid;
      default:
        return TransferStatus::io_error;
    }
  }

  std::size_t FileTransfer::feed(std::span<const std::byte> bytes,
                                 core::Time now) {
    if (reply_size_ || disconnected_ || malformed_ ||
        machine_.state() != State::receiving) {
      return 0;
    }
    const auto count = std::min(bytes.size(), needed_ - received_);
    std::copy_n(bytes.begin(), count, request_.begin() + received_);
    received_ += count;
    if (count) {
      activity_ = now;
    }
    if (received_ == kHeaderBytes) {
      const auto length = util::wire::read32(request_, 12);
      malformed_ = util::wire::read32(request_, 0) != kMagic ||
                   util::wire::read32(request_, 4) != kVersion ||
                   length > kChunkBytes;
      if (!malformed_) {
        needed_ = kHeaderBytes + length;
      }
    }
    return count;
  }

  void FileTransfer::sent(core::Time now) {
    reply_size_ = received_ = 0;
    needed_ = kHeaderBytes;
    malformed_ = false;
    activity_ = now;
  }

  void FileTransfer::Reply(std::size_t payload) {
    util::wire::write32(reply_, 0, kMagic);
    util::wire::write32(reply_, 4, kVersion);
    util::wire::write32(reply_, 8, static_cast<std::uint32_t>(status_));
    util::wire::write32(reply_, 12, static_cast<std::uint32_t>(payload));
    util::wire::write32(reply_, 16, offset_);
    util::wire::write32(reply_, 20, crc_);
    reply_size_ = kHeaderBytes + payload;
  }

  bool FileTransfer::Execute() {
    operated_ = true;
    const auto op = static_cast<Op>(util::wire::read32(request_, 8));
    const auto arg0 = util::wire::read32(request_, 16);
    const auto arg1 = util::wire::read32(request_, 20);
    const auto payload =
        std::span{request_}.subspan(kHeaderBytes, needed_ - kHeaderBytes);
    auto& volume = *access_.volume;
    status_ = TransferStatus::ok;
    const bool mutation =
        op == Op::remove || op == Op::mkdir || op == Op::rmdir;
    if (op == Op::upload || op == Op::download || op == Op::list || mutation) {
      if (leased_) {
        status_ = TransferStatus::invalid;
        return false;
      }
      if (payload.empty() || payload.size() > Volume::kPathCapacity ||
          std::find(payload.begin(), payload.end(), std::byte{0}) !=
              payload.end() ||
          (op != Op::upload && (arg0 || arg1))) {
        status_ = TransferStatus::invalid;
        return false;
      }
      std::copy_n(reinterpret_cast<const char*>(payload.data()), payload.size(),
                  path_.begin());
      path_[payload.size()] = '\0';
      uploading_ = op == Op::upload;
      listing_ = op == Op::list;
      offset_ = crc_ = 0;
      size_ = arg0;
      expected_crc_ = arg1;
      status_ = Map(access_.reserve(access_.context, uploading_ || mutation));
      if (status_ != TransferStatus::ok) {
        return false;
      }
      leased_ = true;
      if (mutation) {
        status_ = Map(op == Op::remove  ? volume.remove(path_.data())
                      : op == Op::mkdir ? volume.mkdir(path_.data())
                                        : volume.rmdir(path_.data()));
        return false;  // Release the lease before acknowledging metadata
                       // changes.
      }
      status_ = Map(uploading_ ? volume.create(path_.data())
                    : listing_ ? volume.open_directory(path_.data())
                               : volume.open(path_.data()));
      if (status_ != TransferStatus::ok) {
        return false;
      }
      opened_ = true;
      created_ = uploading_;
      if (!uploading_) {
        size_ = volume.size();
      }
      Reply();
      // Begin replies advertise the total size rather than a transfer offset.
      util::wire::write32(reply_, 16, size_);
      return true;
    }
    if (!leased_ || (!payload.empty() && op != Op::write)) {
      status_ = TransferStatus::invalid;
      return false;
    }
    if (op == Op::write && uploading_ && !payload.empty() && arg0 == offset_ &&
        !arg1 && payload.size() <= size_ - offset_) {
      std::uint32_t count = 0;
      status_ = Map(
          volume.write({reinterpret_cast<const std::uint8_t*>(payload.data()),
                        payload.size()},
                       count));
      if (status_ == TransferStatus::ok && count != payload.size()) {
        status_ = TransferStatus::io_error;
      }
      if (status_ != TransferStatus::ok) {
        return false;
      }
      crc_ = util::crc32::update(crc_, payload);
      offset_ += count;
      Reply();
      return true;
    }
    if (arg0 || arg1 || !payload.empty()) {
      status_ = TransferStatus::invalid;
      return false;
    }
    if (op == Op::finish && uploading_ && offset_ == size_) {
      if (crc_ != expected_crc_) {
        status_ = TransferStatus::checksum_error;
        return false;
      }
      status_ = Map(volume.sync());
      if (status_ != TransferStatus::ok) {
        return false;
      }
      // Cleanup closes before publishing success. created_ stays true until
      // close succeeds, so a failed close still attempts to remove this file.
      return false;
    }
    if (op == Op::next && listing_) {
      FILINFO entry{};
      status_ = Map(volume.next(entry));
      if (status_ != TransferStatus::ok || !entry.fname[0]) {
        return false;  // End closes the directory and releases the lease.
      }
      const std::string_view name(entry.fname);
      if (name.size() > kChunkBytes - 8) {
        status_ = TransferStatus::io_error;
        return false;
      }
      util::wire::write32(reply_, kHeaderBytes,
                          (entry.fattrib & AM_DIR) ? 1 : 0);
      util::wire::write32(reply_, kHeaderBytes + 4,
                          static_cast<std::uint32_t>(entry.fsize));
      std::copy_n(reinterpret_cast<const std::byte*>(name.data()), name.size(),
                  reply_.begin() + kHeaderBytes + 8);
      Reply(8 + name.size());
      return true;
    }
    if (op == Op::read && !uploading_ && !listing_) {
      auto bytes = std::span{reply_}.subspan(
          kHeaderBytes, std::min<std::size_t>(kChunkBytes, size_ - offset_));
      std::uint32_t count = 0;
      if (!bytes.empty()) {
        status_ = Map(volume.read(
            {reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()},
            count));
      }
      if (status_ == TransferStatus::ok && count != bytes.size()) {
        status_ = TransferStatus::io_error;
      }
      if (status_ != TransferStatus::ok) {
        return false;
      }
      crc_ = util::crc32::update(crc_, bytes);
      offset_ += count;
      // The client sends one final read after receiving all bytes. That reply
      // closes/releases the file and carries its final size and CRC.
      if (!count) {
        return false;
      }
      Reply(count);
      return true;
    }
    status_ = TransferStatus::invalid;
    return false;
  }

  void FileTransfer::Cleanup() {
    operated_ = true;
    bool cleanup_failed = false;
    if (opened_) {
      const auto close = access_.volume->close();
      opened_ = false;
      if (close != FR_OK) {
        cleanup_failed = true;
      }
    }
    if (created_ &&
        (status_ != TransferStatus::ok || disconnected_ || cleanup_failed)) {
      cleanup_failed |= access_.volume->remove(path_.data()) != FR_OK;
    }
    created_ = false;
    if (leased_) {
      access_.release(access_.context);
      leased_ = false;
    }
    if (cleanup_failed) {
      status_ = TransferStatus::cleanup_failed;
    }
    if (status_ != TransferStatus::ok) {
      ++failures_;
    }
    if (disconnected_) {
      sent(activity_);
      disconnected_ = false;
    } else {
      Reply();
    }
  }

  void FileTransfer::Step(State cs, State& ns, core::Time now) {
    operated_ = false;
    const bool expired = (leased_ || received_ || reply_size_) &&
                         now - activity_ >= kIdleTimeout;
    switch (cs) {
      case State::receiving:
        if (disconnected_ || expired || malformed_) {
          status_ = expired ? TransferStatus::timeout : TransferStatus::invalid;
          reply_size_ = 0;
          ns = State::cleanup;
        } else if (!reply_size_ && received_ == needed_) {
          ns = State::executing;
        }
        break;
      case State::executing:
        if (disconnected_ || !access_) {
          status_ = TransferStatus::not_ready;
          ns = State::cleanup;
        } else if (!Execute()) {
          ns = State::cleanup;
        } else {
          ns = State::receiving;
        }
        break;
      case State::cleanup:
        Cleanup();
        ns = State::receiving;
        break;
    }
  }


}  // namespace daveos::storage
