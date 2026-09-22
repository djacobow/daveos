#pragma once

#include <array>
#include <chrono>
#include <cinttypes>
#include <span>
#include <string_view>

#include "confirmation.h"
#include "core/schedule/module.hpp"
#include "util/crc32.h"
#include "util/fault.h"
#include "util/version.h"
#include "watchdog.hpp"

namespace daveos::watchdog {


  // Watchdog supervision as a scheduler module: a heartbeat task and a
  // task-progress check feed the hardware watchdog through Controller, failures
  // are retained across reset, and an optional confirmation callback approves
  // a trial image after it has stayed healthy.
  //
  // Hardware supplies the board's reliability services as static members:
  //   using Watchdog = ...;                      // driver() ->
  //   watchdog::Driver static bool prepare_health(const util::Version&);  //
  //   true after IWDG reset
  //   [[noreturn]] static void initialization_failed();
  //   static util::fault::Record& retained_fault();
  //   static void record_failure(util::fault::Data);
  //   static util::crc32::Backend crc32_backend();
  //
  // Construction is passive. early() (from the composition's early hook,
  // before clock/peripheral setup) binds the scheduler progress source and
  // starts the watchdog; a start failure is terminal.
  template <typename Event, typename Platform, typename Hardware>
  class HealthModule
      : public core::Module<HealthModule<Event, Platform, Hardware>, Event> {
    using Self = HealthModule<Event, Platform, Hardware>;
    static constexpr auto kWatchdogTimeout = std::chrono::seconds{5};
    static constexpr auto kHealthPeriod = std::chrono::milliseconds{100};
    static constexpr auto kHeartbeatPeriod = std::chrono::milliseconds{100};
    static constexpr auto kHeartbeatMaxAge = std::chrono::seconds{1};
    static constexpr auto kProgressAllowance = std::chrono::milliseconds{100};
    static constexpr auto kConfirmationDelay = std::chrono::seconds{5};

   public:
    // version is recorded with every retained failure; borrowed, so it must
    // outlive the module (normally the build's version-stamp constant).
    HealthModule(Platform& platform, const util::Version& version)
        : platform_(platform),
          version_(version),
          checks_{{{"heartbeat", this,
                    [](void* p) {
                      return static_cast<Self*>(p)->HeartbeatHealth();
                    }},
                   {"task_progress", this,
                    [](void* p) {
                      return static_cast<Self*>(p)->ProgressHealth();
                    }}}},
          controller_(hardware_.driver(), checks_,
                      {this, [](void* p, const Failure& f) {
                         static_cast<Self*>(p)->Record(
                             f, util::fault::Kind::watchdog);
                       }}) {}

    static constexpr const char* name() { return "health"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(HealthModule, Heartbeat, kHeartbeatPeriod),
          DAVEOS_PERIODIC(HealthModule, Check, kHealthPeriod)};
    }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(HealthModule, Status, "status",
                                       "Watchdog and confirmation health"),
                        DAVEOS_COMMAND(HealthModule, Fault, "fault",
                                       "Show retained failure diagnostics"),
                        DAVEOS_COMMAND(HealthModule, Clear, "clear",
                                       "Clear retained failure diagnostics"),
                        DAVEOS_COMMAND(HealthModule, Crc, "crc",
                                       "Compare hardware and software CRC32",
                                       core::arg("text"))};
    }

    template <typename Scheduler>
    void attach_progress(Scheduler& scheduler) {
      scheduler_ = &scheduler;
      progress_ = [](void* p) {
        return check_progress(
            static_cast<Scheduler*>(p)->progress(),
            std::chrono::duration_cast<std::chrono::microseconds>(
                kProgressAllowance)
                .count());
      };
    }

    void confirmation(void* context, core::Status (*confirm)(void*)) {
      confirms_image_ = confirm != nullptr;
      confirmation_.callback(context, confirm);
    }

    core::Status early_start() {
      watchdog_reset_ = Hardware::prepare_health(version_);
      return controller_.start(kWatchdogTimeout);
    }

    // Composition's early hook, before HAL/clock setup: bind the progress
    // source, then start the watchdog. A start failure is terminal.
    template <typename Scheduler>
    void early(Scheduler& scheduler) {
      attach_progress(scheduler);
      if (const auto status = early_start(); status != core::Status::ok) {
        initialization_failed(status);
      }
    }

    // Safe before HAL/clock setup; no timer/logger prerequisite.
    [[noreturn]] void initialization_failed(core::Status status,
                                            const char* module = "core") {
      Record({status, "initialization", module},
             util::fault::Kind::initialization);
      Hardware::initialization_failed();
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        last_heartbeat_ = platform_.now();
        if (controller_.state() != Controller::State::running) {
          return core::Status::initialization_failed;
        }
      } else {
        if (watchdog_reset_) {
          W_("Previous reset: IWDG");
        }
        Fault();
      }
      return core::Status::ok;
    }

   private:
    void Heartbeat() { last_heartbeat_ = platform_.now(); }

    Failure HeartbeatHealth() {
      if (!dispatching_) {
        return {};
      }
      const auto maximum =
          std::chrono::duration_cast<std::chrono::microseconds>(
              kHeartbeatMaxAge)
              .count();
      return platform_.now() - last_heartbeat_ >
                     static_cast<core::Time>(maximum)
                 ? Failure{core::Status::health_failed}
                 : Failure{};
    }

    Failure ProgressHealth() {
      if (!dispatching_) {
        return {};
      }
      return progress_ ? progress_(scheduler_)
                       : Failure{core::Status::not_running};
    }

    void Check() {
      if (!dispatching_) {
        last_heartbeat_ = platform_.now();
        dispatching_ = true;
      }
      controller_.tick();
      const auto previous = confirmation_.state();
      auto confirmed = confirmation_.tick(
          platform_.now(), controller_.state() == Controller::State::running);
      if (controller_.state() != Controller::State::running) {
        if (!reported_) {
          reported_ = true;
          E_("Watchdog latched: %s %s.%s", controller_.failure().check,
             controller_.failure().module, controller_.failure().task);
        }
        return;
      }
      if (confirmed != core::Status::ok) {
        initialization_failed(confirmed, "boot");
      }
      if (previous != Confirmation::State::confirmed &&
          confirmation_.state() == Confirmation::State::confirmed) {
        if (confirms_image_) {
          I_("Healthy for five seconds; image confirmed");
        } else {
          I_("Healthy for five seconds");
        }
      }
    }

    void Record(const Failure& failure, util::fault::Kind kind) {
      util::fault::Data data;
      data.kind = kind;
      data.version = version_;
      data.status = static_cast<std::uint32_t>(failure.status);
      util::fault::copy_name(data.check, failure.check);
      util::fault::copy_name(data.module,
                             failure.module ? failure.module : "core");
      util::fault::copy_name(data.task, failure.task);
      data.expected = failure.expected;
      data.completed = failure.completed;
      Hardware::record_failure(data);
    }

    core::Status Status() {
      I_("Watchdog %s; confirmation %s",
         controller_.state() == Controller::State::running ? "running"
                                                           : "latched",
         !confirms_image_ ? "not configured"
         : confirmation_.state() == Confirmation::State::confirmed ? "confirmed"
                                                                   : "waiting");
      return core::Status::ok;
    }

    core::Status Fault() {
      util::fault::Data data;
      if (util::fault::read(Hardware::retained_fault(), data)) {
        W_("Retained failure: %s %s.%s status %s", data.check, data.module,
           data.task, enum_name(static_cast<core::Status>(data.status)));
        W_("CFSR=%08" PRIx32 " HFSR=%08" PRIx32 " SP=%08" PRIx32, data.cfsr,
           data.hfsr, data.stack);
        if (data.frame_valid) {
          W_("Frame PC=%08" PRIx32 " LR=%08" PRIx32 " xPSR=%08" PRIx32,
             data.frame[6], data.frame[5], data.frame[7]);
        } else {
          W_("Exception frame unavailable");
        }
      } else {
        I_("No retained failure");
      }
      return core::Status::ok;
    }

    core::Status Crc(std::string_view text) {
      const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
      const util::crc32::Service service(Hardware::crc32_backend());
      const auto value = service.calculate(bytes);
      const auto split = bytes.size() / 2;
      const auto incremental = service.update(
          service.calculate(bytes.first(split)), bytes.subspan(split));
      if (value != util::crc32::calculate(bytes) || value != incremental) {
        return core::Status::checksum_error;
      }
      I_("CRC32 %08" PRIx32 " (hardware/software/incremental agree)", value);
      return core::Status::ok;
    }

    core::Status Clear() {
      util::fault::clear(Hardware::retained_fault());
      I_("Retained failure cleared");
      return core::Status::ok;
    }

    Platform& platform_;
    const util::Version& version_;
    typename Hardware::Watchdog hardware_;
    std::array<watchdog::Check, 2> checks_;
    Controller controller_;
    void* scheduler_ = nullptr;
    Failure (*progress_)(void*) = nullptr;
    Confirmation confirmation_{kConfirmationDelay};
    core::Time last_heartbeat_ = 0;
    bool watchdog_reset_ = false;
    bool dispatching_ = false, reported_ = false, confirms_image_ = false;
  };


}  // namespace daveos::watchdog
