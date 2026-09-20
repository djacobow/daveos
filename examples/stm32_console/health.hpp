#pragma once

#include <cinttypes>

#include "application.h"
#include "platform/stm32h5/reliability.h"
#include "stm32h563xx.h"
#include "util/version_stamp.h"
#include "watchdog/confirmation.h"
#include "watchdog/watchdog.hpp"

namespace app {


  using std::chrono_literals::operator""ms;
  using std::chrono_literals::operator""s;
  inline constexpr auto kWatchdogTimeout = 5s;
  inline constexpr auto kHealthPeriod = 100ms;
  inline constexpr auto kHeartbeatPeriod = 100ms;
  inline constexpr auto kHeartbeatMaxAge = 1s;
  inline constexpr auto kProgressAllowance = 100ms;
  inline constexpr auto kConfirmationDelay = 5s;
  inline constexpr std::uint32_t kFailureUartBudget = 64000;

  namespace wd = daveos::watchdog;
  namespace fault = daveos::util::fault;
  namespace h5 = daveos::platform::stm32h5;

  // All hardware work waits for explicit early_start()/module initialization.
  // The scheduler callback is bound after static construction has completed.
  class Health : public core::Module<Health, Event> {
   public:
    explicit Health(Platform& platform)
        : platform_(platform),
          checks_{{{"heartbeat", this,
                    [](void* p) {
                      return static_cast<Health*>(p)->HeartbeatHealth();
                    }},
                   {"task_progress", this,
                    [](void* p) {
                      return static_cast<Health*>(p)->ProgressHealth();
                    }}}},
          controller_(hardware_.driver(), checks_,
                      {this, [](void* p, const wd::Failure& f) {
                         static_cast<Health*>(p)->Record(f,
                                                         fault::Kind::watchdog);
                       }}) {}

    static constexpr const char* name() { return "health"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_PERIODIC(Health, Heartbeat, kHeartbeatPeriod),
                        DAVEOS_PERIODIC(Health, Check, kHealthPeriod)};
    }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Health, Status, "status",
                                       "Watchdog and confirmation health"),
                        DAVEOS_COMMAND(Health, Fault, "fault",
                                       "Show retained failure diagnostics"),
                        DAVEOS_COMMAND(Health, Clear, "clear",
                                       "Clear retained failure diagnostics")};
    }

    template <typename Scheduler>
    void attach_progress(Scheduler& scheduler) {
      scheduler_ = &scheduler;
      progress_ = [](void* p) {
        return wd::check_progress(
            static_cast<Scheduler*>(p)->progress(),
            std::chrono::duration_cast<std::chrono::microseconds>(
                kProgressAllowance)
                .count());
      };
    }

    void confirmation(void* context, core::Status (*confirm)(void*)) {
      confirmation_.callback(context, confirm);
    }

    core::Status early_start() {
      // Pause the health clock along with IWDG when debugging a halted CPU.
      DBGMCU->APB1FZR1 = DBGMCU->APB1FZR1 | DBGMCU_APB1FZR1_DBG_TIM2_STOP;
      h5::set_fault_identity(daveos::build::kApplicationVersion, 0);
      reset_cause_ = RCC->RSR;
      RCC->RSR = RCC->RSR | RCC_RSR_RMVF;
      return controller_.start(kWatchdogTimeout);
    }

    // Safe before HAL/clock setup; no timer/logger prerequisite.
    [[noreturn]] void initialization_failed(core::Status status,
                                            const char* module = "core") {
      Record({status, "initialization", module}, fault::Kind::initialization);
      if ((RCC->APB1LENR & RCC_APB1LENR_USART3EN) &&
          (USART3->CR1 & (USART_CR1_UE | USART_CR1_TE)) ==
              (USART_CR1_UE | USART_CR1_TE)) {
        for (char byte :
             std::string_view("DaveOS initialization failed; resetting\r\n")) {
          auto budget = kFailureUartBudget;
          while (!(USART3->ISR & USART_ISR_TXE_TXFNF) && --budget) {
            __NOP();
          }
          if (!budget) {
            break;
          }
          USART3->TDR = static_cast<std::uint8_t>(byte);
        }
      }
      __DSB();
      h5::reset();
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        last_heartbeat_ = platform_.now();
        if (controller_.state() != wd::Controller::State::running) {
          return core::Status::initialization_failed;
        }
      } else {
        if (reset_cause_ & RCC_RSR_IWDGRSTF) {
          W_("Previous reset: IWDG");
        }
        Fault();
      }
      return core::Status::ok;
    }

   private:
    void Heartbeat() { last_heartbeat_ = platform_.now(); }

    wd::Failure HeartbeatHealth() {
      if (!dispatching_) {
        return {};
      }
      const auto maximum =
          std::chrono::duration_cast<std::chrono::microseconds>(
              kHeartbeatMaxAge)
              .count();
      return platform_.now() - last_heartbeat_ >
                     static_cast<core::Time>(maximum)
                 ? wd::Failure{core::Status::health_failed}
                 : wd::Failure{};
    }

    wd::Failure ProgressHealth() {
      if (!dispatching_) {
        return {};
      }
      return progress_ ? progress_(scheduler_)
                       : wd::Failure{core::Status::not_running};
    }

    void Check() {
      if (!dispatching_) {
        last_heartbeat_ = platform_.now();
        dispatching_ = true;
      }
      controller_.tick();
      const auto previous = confirmation_.state();
      auto confirmed = confirmation_.tick(
          platform_.now(),
          controller_.state() == wd::Controller::State::running);
      if (controller_.state() != wd::Controller::State::running) {
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
      if (previous != wd::Confirmation::State::confirmed &&
          confirmation_.state() == wd::Confirmation::State::confirmed) {
        I_("Healthy for five seconds; image confirmed");
      }
    }

    void Record(const wd::Failure& failure, fault::Kind kind) {
      fault::Data data;
      data.kind = kind;
      data.version = daveos::build::kApplicationVersion;
      data.status = static_cast<std::uint32_t>(failure.status);
      fault::copy_name(data.check, failure.check);
      fault::copy_name(data.module, failure.module ? failure.module : "core");
      fault::copy_name(data.task, failure.task);
      data.expected = failure.expected;
      data.completed = failure.completed;
      h5::record_failure(data);
    }

    core::Status Status() {
      I_("Watchdog %s; confirmation %s",
         controller_.state() == wd::Controller::State::running ? "running"
                                                               : "latched",
         confirmation_.state() == wd::Confirmation::State::confirmed
             ? "confirmed"
             : "waiting");
      return core::Status::ok;
    }

    core::Status Fault() {
      fault::Data data;
      if (fault::read(h5::retained_fault(), data)) {
        W_("Retained failure: %s %s.%s status %s", data.check, data.module,
           data.task, enum_name(static_cast<core::Status>(data.status)));
        if (data.frame_valid) {
          W_("Frame PC=%08" PRIx32 " LR=%08" PRIx32 " xPSR=%08" PRIx32,
             data.frame[6], data.frame[5], data.frame[7]);
        }
      } else {
        I_("No retained failure");
      }
      return core::Status::ok;
    }

    core::Status Clear() {
      fault::clear(h5::retained_fault());
      I_("Retained failure cleared");
      return core::Status::ok;
    }

    Platform& platform_;
    h5::Watchdog hardware_;
    std::array<wd::Check, 2> checks_;
    wd::Controller controller_;
    void* scheduler_ = nullptr;
    wd::Failure (*progress_)(void*) = nullptr;
    wd::Confirmation confirmation_{kConfirmationDelay};
    core::Time last_heartbeat_ = 0;
    std::uint32_t reset_cause_ = 0;
    bool dispatching_ = false, reported_ = false;
  };


}  // namespace app
