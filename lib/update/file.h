#pragma once

#include <string_view>

#include "engine.h"
#include "storage/read_file.h"

namespace daveos::update {


  using FileSource = storage::ReadFile;

  // One owning task; public methods only reserve resources/submit requests.
  // The caller ticks Engine separately. Prepared files have no expiry. Keep
  // this object, source and engine alive until busy() becomes false.
  class FileUpdate {
   public:
    enum class State {
      idle,
      opening,
      header,
      validating,
      eof,
      prepared,
      rewind,
      checking,
      starting,
      installing,
      cleanup,
      done,
      failed
    };
    static constexpr std::size_t kPathCapacity = 255;

    FileUpdate(Engine& engine, const FileSource& source)
        : engine_(engine), source_(source) {}

    FileUpdate(const FileUpdate&) = delete;
    FileUpdate& operator=(const FileUpdate&) = delete;
    FileUpdate(FileUpdate&&) = delete;
    FileUpdate& operator=(FileUpdate&&) = delete;


    Status prepare(std::string_view path);
    Status install();

    void cancel() {
      // A completed durable commit cannot be cancelled retroactively.
      if (busy_ && state() != State::cleanup &&
          !(installing_ && engine_.state() == Engine::State::done)) {
        cancel_requested_ = true;
      }
    }

    void tick() { (void)machine_.tick(*this); }

    bool busy() const { return busy_; }

    State state() const { return machine_.state(); }

    const char* phase() const;

    Status status() const { return status_; }

    const char* path() const { return path_.data(); }

    const Header& header() const { return header_; }

    std::uint32_t destination() const { return engine_.destination(); }

    std::uint32_t progress() const {
      return installing_ ? installed_ : received_;
    }

    std::uint32_t total() const {
      return header_.package_size >= kHeaderSize
                 ? header_.package_size - kHeaderSize
                 : 0;
    }

   private:
    class Machine
        : public core::StateMachine<Machine, State, State::idle,
                                    static_cast<std::size_t>(State::failed) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::idle,
                                      static_cast<std::size_t>(State::failed) +
                                          1>;

      void Step(State cs, State& ns, FileUpdate& owner) { owner.Step(cs, ns); }
    };

    void Step(State cs, State& ns);
    bool Read(std::span<std::byte> bytes, std::uint32_t& count);
    Engine& engine_;
    FileSource source_;
    Machine machine_;
    PackageReader reader_;
    Header header_{};
    std::array<char, kPathCapacity + 1> path_{};
    std::array<std::byte, kHeaderSize> header_bytes_{};
    std::array<std::byte, kBlockSize> buffer_{};
    std::size_t filled_ = 0, at_ = 0;
    std::uint32_t received_ = 0, crc_ = 0, installed_ = 0;
    Status status_ = Status::not_running;
    bool busy_ = false, opened_ = false, requested_ = false;
    bool install_requested_ = false, cancel_requested_ = false,
         installing_ = false;
  };


}  // namespace daveos::update
