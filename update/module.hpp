#pragma once

#include <cinttypes>

#include "core/schedule/module.hpp"
#include "file.h"
#include "protocol.h"

namespace daveos::update {


  // Optional scheduler/command adapter. The engine and transport remain usable
  // without DaveOS. Transport polling is supplied after passive construction.
  template <typename Event = core::NoEvent, bool Files = false>
  class Module : public core::Module<Module<Event, Files>, Event> {
   public:
    explicit Module(Engine& engine, FileSource source = {})
        : engine_(engine), file_(engine, source) {}

    static constexpr const char* name() { return "ota"; }

    static constexpr auto tasks() {
      if constexpr (Files) {
        return std::array{
            DAVEOS_PERIODIC(Module, Poll, std::chrono::milliseconds{1}),
            DAVEOS_TASK(Module, FilePoll)};
      } else {
        return std::array{
            DAVEOS_PERIODIC(Module, Poll, std::chrono::milliseconds{1})};
      }
    }

    static constexpr auto commands() {
      constexpr auto common = std::array{
          DAVEOS_COMMAND(Module, Enable, "enable",
                         "Enable firmware uploads until reset"),
          DAVEOS_COMMAND(Module, Disable, "disable",
                         "Disable uploads and abort incomplete installation"),
          DAVEOS_COMMAND(Module, Report, "status",
                         "Report firmware update progress")};
      if constexpr (Files) {
        return std::array{
            common[0], common[1], common[2],
            DAVEOS_COMMAND(
                Module, Prepare, "init",
                "Validate and reserve an SD package (mount read-only first)",
                core::arg("path")),
            DAVEOS_COMMAND(
                Module, Install, "install",
                "Install the prepared SD package; reset explicitly afterward")};
      } else {
        return common;
      }
    }

    void transport(void* context, void (*poll)(void*)) {
      context_ = context;
      poll_ = poll;
    }

    void setup(void* context, core::Status (*initialize)(void*)) {
      setup_context_ = context;
      initialize_ = initialize;
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage1 && initialize_
                 ? initialize_(setup_context_)
                 : core::Status::ok;
    }

   private:
    void Poll() {
      engine_.tick();
      if constexpr (Files) {
        if (engine_.active() && !file_.busy()) {
          network_since_file_ = true;
        }
      }
      if (poll_) {
        poll_(context_);
      }
    }

    core::Status Enable() {
      engine_.enable(true);
      I_("OTA enabled");
      return core::Status::ok;
    }

    core::Status Disable() {
      if constexpr (Files) {
        if (file_.busy()) {
          const auto result = ScheduleFile();
          if (result != core::Status::ok) {
            return result;
          }
          file_.cancel();
        }
      }
      engine_.enable(false);
      I_("OTA disabled");
      return core::Status::ok;
    }

    core::Status Report() {
      if constexpr (Files) {
        if (file_.busy() || (!network_since_file_ && !engine_.active() &&
                             file_.state() != FileUpdate::State::idle)) {
          FileReport();
          return core::Status::ok;
        }
      }
      I_("OTA %s: %s; received %" PRIu32 "/%" PRIu32 "; source TCP",
         state_name(engine_.state()), enum_name(engine_.status()),
         engine_.next_offset(), engine_.total());
      return core::Status::ok;
    }

    core::Status ScheduleFile() {
      return this->template schedule<&Module::FilePoll>(
          std::chrono::microseconds{0});
    }

    core::Status Prepare(std::string_view path) {
      auto result = ScheduleFile();
      if (result != core::Status::ok) {
        return result;
      }
      result = file_.prepare(path);
      if (result == core::Status::not_running ||
          result == core::Status::rejected) {
        E_("SD update requires a read-only mount; unmount a read-write volume "
           "first");
      }
      if (result == core::Status::ok) {
        network_since_file_ = false;
        I_("OTA SD validating: %s", file_.path());
      }
      return result;
    }

    core::Status Install() {
      auto result = ScheduleFile();
      return result == core::Status::ok ? file_.install() : result;
    }

    void FileReport() {
      I_("OTA SD %s: %s; %" PRIu32 "/%" PRIu32 "; %s", file_.phase(),
         enum_name(file_.status()), file_.progress(), file_.total(),
         file_.path());
    }

    void FilePoll() {
      if constexpr (Files) {
        if (!file_.busy()) {
          return;
        }
        const auto previous = file_.state();
        file_.tick();
        if (file_.state() != previous &&
            file_.state() == FileUpdate::State::prepared) {
          const auto& h = file_.header();
          I_("OTA SD prepared: version %" PRIu32 ".%" PRIu32 ".%" PRIu32
             " product %" PRIu32 " layout %" PRIu32 " image %" PRIu32
             " package %" PRIu32 " slot %c",
             h.version.major, h.version.minor, h.version.build, h.product,
             h.revision, h.image_size, h.package_size,
             'A' + static_cast<int>(file_.destination()));
        }
        // Bounded progress output, including verification/commit phases.
        if (!file_.busy() || ++report_ticks_ >= kReportTicks) {
          report_ticks_ = 0;
          FileReport();
        }
        if (file_.busy() && file_.state() != FileUpdate::State::prepared) {
          const auto result = this->template schedule<&Module::FilePoll>(
              std::chrono::milliseconds{1});
          if (result != core::Status::ok) {
            file_.cancel();
            E_("OTA SD worker: %s", enum_name(result));
          }
        }
      }
    }

    struct NoFile {
      NoFile(Engine&, FileSource) {}
    };

    static constexpr std::uint32_t kReportTicks = 1000;
    Engine& engine_;
    [[no_unique_address]] std::conditional_t<Files, FileUpdate, NoFile> file_;
    std::uint32_t report_ticks_ = 0;
    bool network_since_file_ = false;
    void* context_ = nullptr;
    void (*poll_)(void*) = nullptr;
    void* setup_context_ = nullptr;
    core::Status (*initialize_)(void*) = nullptr;
  };


}  // namespace daveos::update
