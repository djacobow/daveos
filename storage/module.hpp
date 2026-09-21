#pragma once

#include <algorithm>
#include <cinttypes>
#include <optional>

#include "core/schedule/module.hpp"
#include "core/state_machine/state_machine.hpp"
#include "volume.h"

namespace daveos::storage {


  // One request at a time. Command handlers only copy arguments and schedule
  // the one-shot worker: FatFs calls can yield from that worker, never from a
  // command handler. Returning between entries/chunks lets logs and events
  // drain. The worker is not periodic, so a slow card is not a missed
  // heartbeat.
  template <typename Event = core::NoEvent>
  class Module : public core::Module<Module<Event>, Event> {
    static constexpr auto kContinueDelay = std::chrono::milliseconds{1};
    static constexpr std::uint32_t kDefaultRead = 128;
    static constexpr std::uint32_t kMaximumRead = 4096;
    static constexpr std::uint32_t kMaximumEntries = 256;

   public:
    explicit Module(const BlockDevice& device) : volume_(device) {}

    static constexpr const char* name() { return "fs"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Module, Poll)};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Module, Mount, "mount",
                         "Mount read-only media; run sd probe first"),
          DAVEOS_COMMAND(Module, Unmount, "unmount",
                         "Unmount and release media"),
          DAVEOS_COMMAND(Module, List, "ls",
                         "List directory (up to 256 entries)",
                         core::arg("path")),
          DAVEOS_COMMAND(Module, Read, "read",
                         "Hex/ASCII file preview (default 128 bytes)",
                         core::arg("path"), core::arg("offset"),
                         core::arg("count").range(1u, kMaximumRead))};
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage1 && volume_.attach() != FR_OK
                 ? core::Status::initialization_failed
                 : core::Status::ok;
    }

    core::Status Mount() { return Submit(Operation::mount, {}); }

    core::Status Unmount() { return Submit(Operation::unmount, {}); }

    core::Status List(std::optional<std::string_view> path) {
      return Submit(Operation::list, path.value_or("/"));
    }

    core::Status Read(std::string_view path,
                      std::optional<std::uint32_t> offset,
                      std::optional<std::uint32_t> count) {
      if (busy_) {
        return core::Status::busy;
      }
      if (count && (*count == 0 || *count > kMaximumRead)) {
        return core::Status::invalid_argument;
      }
      offset_ = offset.value_or(0);
      remaining_ = count.value_or(kDefaultRead);
      return Submit(Operation::read, path);
    }

   private:
    enum class Operation { mount, unmount, list, read };
    enum class State {
      idle,
      mount,
      unmount,
      open_directory,
      list,
      open_file,
      read,
      close,
      done,
      failed,
      count
    };

    struct Machine
        : core::StateMachine<Machine, State, State::idle,
                             static_cast<std::size_t>(State::count)> {
      void Step(State cs, State& ns, Module& m) {
        switch (cs) {
          case State::idle:
          case State::done:
          case State::failed:
            if (m.requested_) {
              m.requested_ = false;
              m.entries_ = 0;
              switch (m.operation_) {
                case Operation::mount:
                  ns = State::mount;
                  break;
                case Operation::unmount:
                  ns = State::unmount;
                  break;
                case Operation::list:
                  ns = State::open_directory;
                  break;
                case Operation::read:
                  ns = State::open_file;
                  break;
              }
            }
            break;
          case State::mount:
            m.result_ = m.volume_.mount();
            ns = m.result_ == FR_OK ? State::done : State::failed;
            break;
          case State::unmount:
            m.result_ = m.volume_.unmount();
            ns = m.result_ == FR_OK ? State::done : State::failed;
            break;
          case State::open_directory:
            m.result_ = m.volume_.open_directory(m.path_.data());
            ns = m.result_ == FR_OK ? State::list : State::failed;
            break;
          case State::list: {
            FILINFO entry{};
            m.result_ = m.volume_.next(entry);
            if (m.result_ != FR_OK) {
              ns = State::failed;
            } else if (!entry.fname[0]) {
              ns = State::close;
            } else {
              m.Entry(entry);
              if (++m.entries_ == kMaximumEntries) {
                m.Limit();
                ns = State::close;
              }
            }
            break;
          }
          case State::open_file:
            m.result_ = m.volume_.open(m.path_.data(), m.offset_);
            ns = m.result_ == FR_OK ? State::read : State::failed;
            break;
          case State::read: {
            std::array<std::uint8_t, 16> bytes{};
            std::uint32_t count = 0;
            m.result_ =
                m.volume_.read(std::span{bytes}.first(std::min<std::size_t>(
                                   bytes.size(), m.remaining_)),
                               count);
            if (m.result_ != FR_OK) {
              ns = State::failed;
            } else {
              if (count) {
                m.Bytes(std::span{bytes}.first(count));
              }
              m.offset_ += count;
              m.remaining_ -= count;
              if (!count || !m.remaining_) {
                ns = State::close;
              }
            }
            break;
          }
          case State::close:
            m.result_ = m.volume_.close();
            ns = m.result_ == FR_OK ? State::done : State::failed;
            break;
          case State::count:
            break;
        }
      }

      void OnEnter(State state, Module& m) {
        if (state == State::done || state == State::failed) {
          if (state == State::failed) {
            (void)m.volume_.close();
          }
          m.Finish();
          m.busy_ = false;
        }
      }
    };

    core::Status Submit(Operation operation, std::string_view path) {
      if (busy_) {
        return core::Status::busy;
      }
      if (path.size() > Volume::kPathCapacity || path.find('\0') != path.npos ||
          path.find(':') != path.npos) {
        return core::Status::invalid_argument;
      }
      std::copy(path.begin(), path.end(), path_.begin());
      path_[path.size()] = '\0';
      operation_ = operation;
      requested_ = busy_ = true;
      const auto status =
          this->template schedule<&Module::Poll>(std::chrono::microseconds{0});
      if (status != core::Status::ok) {
        requested_ = busy_ = false;
      }
      return status;
    }

    void Poll() {
      (void)machine_.tick(*this);
      if (busy_ && this->template schedule<&Module::Poll>(kContinueDelay) !=
                       core::Status::ok) {
        (void)volume_.close();
        requested_ = busy_ = false;
      }
    }

    void Entry(FILINFO& entry) {
      for (char& c : entry.fname) {
        if (!c) {
          break;
        }
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
          c = '?';
        }
      }
      I_("%c %" PRIu32 " %s", entry.fattrib & AM_DIR ? 'd' : 'f',
         static_cast<std::uint32_t>(entry.fsize), entry.fname);
    }

    void Bytes(std::span<const std::uint8_t> bytes) {
      constexpr char digits[] = "0123456789abcdef";
      std::array<char, 33> hex{};
      std::array<char, 17> ascii{};
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        hex[2 * i] = digits[bytes[i] >> 4];
        hex[2 * i + 1] = digits[bytes[i] & 15];
        ascii[i] = bytes[i] >= 32 && bytes[i] <= 126
                       ? static_cast<char>(bytes[i])
                       : '.';
      }
      I_("%08" PRIx32 "  %s  %s", offset_, hex.data(), ascii.data());
    }

    void Limit() {
      W_("Directory listing limited to %" PRIu32 " entries", kMaximumEntries);
    }

    void Finish() {
      if (result_ != FR_OK) {
        E_("Filesystem: %s", result_name(result_));
      } else if (operation_ == Operation::mount) {
        I_("Filesystem mounted read-only");
      } else if (operation_ == Operation::unmount) {
        I_("Filesystem unmounted");
      } else {
        I_("Filesystem request complete");
      }
    }

    Volume volume_;
    Machine machine_;
    std::array<char, Volume::kPathCapacity + 1> path_{};
    Operation operation_ = Operation::mount;
    FRESULT result_ = FR_OK;
    std::uint32_t offset_ = 0, remaining_ = 0, entries_ = 0;
    bool busy_ = false, requested_ = false;
  };


}  // namespace daveos::storage
