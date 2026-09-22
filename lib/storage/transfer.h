#pragma once

#include "core/state_machine/state_machine.hpp"
#include "volume.h"

namespace daveos::storage {


  // Borrowed filesystem lease. reserve(write) excludes commands and other file
  // users for the entire transfer, including across cooperative yields. Reads
  // permit either mount mode; writes require rw. Only release a held lease.
  struct FileAccess {
    Volume* volume = nullptr;
    void* context = nullptr;
    FRESULT (*reserve)(void*, bool write) = nullptr;
    void (*release)(void*) = nullptr;

    explicit operator bool() const { return volume && reserve && release; }
  };

#define DAVEOS_TRANSFER_STATUS(X) \
  X(ok)                           \
  X(invalid)                      \
  X(busy)                         \
  X(not_ready)                    \
  X(read_only)                    \
  X(exists)                       \
  X(not_found)                    \
  X(io_error)                     \
  X(checksum_error)               \
  X(timeout)                      \
  X(cleanup_failed)               \
  X(denied)
  DAVEOS_ENUM(TransferStatus, std::uint32_t, DAVEOS_TRANSFER_STATUS)
#undef DAVEOS_TRANSFER_STATUS

  // DOSF v1, described in docs/file-transfer.md. One stop-and-wait request at a
  // time; fixed buffers, no network dependency. feed() only copies input;
  // tick() executes one protocol request or cleanup step; either may make
  // several filesystem calls.
  // Call tick only from a task: file operations can yield. All access is
  // serialized. disconnect() requests cleanup; keep ticking until busy() is
  // false before destroying this object or its borrowed filesystem. Power loss
  // cannot clean up partial files. The inactivity timeout excludes time spent
  // inside I/O.
  class FileTransfer {
   public:
    static constexpr std::uint32_t kMagic = 0x46534f44;
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::size_t kHeaderBytes = 24;
    static constexpr std::size_t kChunkBytes = 1024;
    static constexpr core::Time kIdleTimeout = 30000000;
    enum class Op : std::uint32_t {
      upload = 1,
      write,
      finish,
      download,
      read,
      list,
      next,
      remove,
      mkdir,
      rmdir
    };

    explicit FileTransfer(const FileAccess& access) : access_(access) {}

    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    std::size_t feed(std::span<const std::byte> bytes, core::Time now);

    void tick(core::Time now) { (void)machine_.tick(*this, now); }

    // Record actual completion time after tick(), which may yield for I/O.
    void completed_at(core::Time now) {
      if (operated_) {
        activity_ = now;
      }
    }

    void disconnect() { disconnected_ = true; }

    std::span<const std::byte> reply() const {
      return {reply_.data(), disconnected_ ? 0 : reply_size_};
    }

    void sent(core::Time now);

    bool busy() const { return leased_ || received_ || disconnected_; }

    TransferStatus status() const { return status_; }

    const char* path() const { return path_.data(); }

    std::uint32_t failures() const { return failures_; }

   private:
    enum class State { receiving, executing, cleanup };

    class Machine
        : public core::StateMachine<Machine, State, State::receiving, 3> {
      friend class core::StateMachine<Machine, State, State::receiving, 3>;

      void Step(State cs, State& ns, FileTransfer& owner, core::Time now) {
        owner.Step(cs, ns, now);
      }
    };

    void Step(State cs, State& ns, core::Time now);
    bool Execute();
    void Cleanup();
    void Reply(std::size_t payload = 0);
    static TransferStatus Map(FRESULT result);
    FileAccess access_;
    Machine machine_;
    std::array<std::byte, kHeaderBytes + kChunkBytes> request_{}, reply_{};
    std::array<char, Volume::kPathCapacity + 1> path_{};
    std::size_t received_ = 0, needed_ = kHeaderBytes, reply_size_ = 0;
    core::Time activity_ = 0;
    std::uint32_t size_ = 0, offset_ = 0, crc_ = 0, expected_crc_ = 0,
                  failures_ = 0;
    TransferStatus status_ = TransferStatus::ok;
    bool leased_ = false, opened_ = false, uploading_ = false, created_ = false;
    bool listing_ = false;
    bool disconnected_ = false, malformed_ = false, operated_ = false;
  };


}  // namespace daveos::storage
