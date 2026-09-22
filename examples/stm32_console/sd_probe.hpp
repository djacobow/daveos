#pragma once

#include <algorithm>
#include <atomic>
#include <cinttypes>

#include "application.h"
#include "core/state_machine/state_machine.hpp"
#include "hal/adapters/daveos.hpp"
#include "hal/controller.hpp"
#include "sd_board.h"
#include "sd_inspect.h"
#include "storage/sd/initializer.h"
#include "storage/sd/transport.h"

namespace app {


  namespace hal = daveos::hal;
  namespace spi = hal::spi;

  // Read-only SPI fixture: identify the card, verify reads and inspect its BPB.
  // No block writes or filesystem changes in the probe. The periodic task owns
  // all protocol state; interrupt callbacks only publish the HAL completion
  // result.
  class SdProbe : public core::Module<SdProbe, Event> {
    using Clock = hal::DaveOsClock<core::SchedulerInterface<Event>, Platform>;

    // Dedicated fixture backend: keep a private config copy so startup can
    // use <=400 kHz and completed transactions can switch to a conservative
    // data rate. Only this module owns the bus; speed changes occur in Tick
    // after completion, never while a HAL operation owns the peripheral.
    class SdBus : public board::SdSpiBus {
     public:
      explicit SdBus(const daveos::platform::stm32::detail::SpiDma& dma)
          : board::SdSpiBus(SPI1, SPI1_IRQn, HSI_VALUE,
                            {nullptr, Prepare, Reset, nullptr}, dma) {}

      hal::Status init(std::span<const Config> devices) {
        configs_[0] = devices[0];
        configs_[0].maximum_hz = kStartupHz;
        return board::SdSpiBus::init(configs_);
      }

      void speed(std::uint32_t hz) { configs_[0].maximum_hz = hz; }

     private:
      std::array<Config, 1> configs_{};
    };

    using Bus = hal::Controller<SdBus, Clock, board::SdBusCritical, 1>;
    static constexpr std::uint32_t kStartupHz = 400000;
    static constexpr std::uint32_t kReadHz = 1000000;
    static constexpr std::uint32_t kReadRepeats = 3;
    static constexpr std::size_t kSectorSize = 512;
    // R1, data token, one sector and its CRC; no fixed token-wait window.
    static constexpr std::size_t kReceiveCapacity = kSectorSize + 4;
    static constexpr auto kPeriod = std::chrono::milliseconds{1};
    static constexpr auto kTransferTimeout = std::chrono::milliseconds{250};

   public:
    explicit SdProbe(Platform& platform, bool dma = false)
        : clock_(platform),
          dma_(dma),
          backend_(dma_.operations()),
          bus_(backend_, clock_, critical_,
               {{{{GPIOD, GPIO_PIN_14, false}, kReadHz, 0, false}}}),
          initializer_(bus_.device<0>()) {}

    static constexpr const char* name() { return "sd"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_PERIODIC(SdProbe, Tick, kPeriod)};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(SdProbe, Probe, "probe",
                         "Identify SD, CRC-check sectors and inspect "
                         "filesystem (read-only)"),
          DAVEOS_COMMAND(SdProbe, ResetBus, "reset",
                         "Reset SPI controller; unmount first, then sd probe"),
          DAVEOS_COMMAND(SdProbe, Stats, "stats",
                         "SPI polls and DMA payload counters (since startup)")};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        clock_.bind(scheduler());
        return bus_.init() == hal::Status::ok
                   ? core::Status::ok
                   : core::Status::initialization_failed;
      }
      return core::Status::ok;
    }

    void interrupt() {
      ++interrupts_;
      bus_.interrupt();
    }

    core::Status Probe() {
      if (running_ || requested_ || reset_requested_ || leased_) {
        return core::Status::busy;
      }
      ready_ = false;
      requested_ = true;
      return core::Status::ok;
    }

    core::Status ResetBus() {
      if (running_ || requested_ || reset_requested_ || leased_) {
        return core::Status::busy;
      }
      ready_ = false;
      reset_requested_ = true;
      return core::Status::ok;
    }

    core::Status Stats() {
      critical_.enter();
      const auto counters = backend_.counters();
      const auto interrupts = interrupts_;
      critical_.leave();
      I_("SPI1 IRQs=%" PRIu32 " polls=%" PRIu32 " DMA chunks=%" PRIu32
         " bytes=%" PRIu32,
         interrupts, counters.polls, counters.dma_chunks, counters.dma_bytes);
      I_("SPI1 faulted=%s; SD ready=%s; last error=%s",
         bus_.faulted() ? "yes" : "no", ready_ ? "yes" : "no", error_);
      return core::Status::ok;
    }

    // File-scope wiring only takes our address. No hardware or card access.
    daveos::storage::BlockDevice block_device() {
      return {this,
              [](void* p) { return static_cast<SdProbe*>(p)->ready_; },
              [](void* p) { return static_cast<SdProbe*>(p)->card_.sectors; },
              [](void* p, std::uint32_t sector, std::span<std::uint8_t> bytes) {
                auto& self = *static_cast<SdProbe*>(p);
                if (!self.ready_ || !self.leased_) {
                  return false;
                }
                auto reader = self.Transport();
                if (!reader.read(sector, bytes)) {
                  self.error_ = enum_name(reader.status());
                  self.ReadFailure(sector);
                  self.ready_ = false;
                  return false;
                }
                return true;
              },
              [](void* p) {
                auto& self = *static_cast<SdProbe*>(p);
                if (!self.ready_ || self.leased_ || self.running_ ||
                    self.requested_) {
                  return false;
                }
                self.leased_ = true;
                return true;
              },
              [](void* p) { static_cast<SdProbe*>(p)->leased_ = false; },
              [](void* p, std::uint32_t sector,
                 std::span<const std::uint8_t> bytes) {
                auto& self = *static_cast<SdProbe*>(p);
                if (!self.ready_ || !self.leased_) {
                  return false;
                }
                auto transport = self.Transport();
                if (!transport.write(sector, bytes)) {
                  self.error_ = enum_name(transport.status());
                  self.WriteFailure(sector, transport.write_diagnostics());
                  self.ready_ = false;
                  return false;
                }
                return true;
              },
              [](void* p) {
                const auto& self = *static_cast<SdProbe*>(p);
                // Every successful sector write waits for ready and CMD13.
                return self.ready_ && self.leased_;
              }};
    }

    void Tick() { machine_.tick(*this); }

   private:
    void ReadFailure(std::uint32_t sector) {
      E_("SD read LBA %" PRIu32 ": %s; reinitialization required", sector,
         error_);
    }

    void WriteFailure(
        std::uint32_t sector,
        const daveos::storage::sd::Transport::WriteDiagnostics& d) {
      E_("SD write LBA %" PRIu32 " CMD%" PRIu32 ": %s after %" PRIu32
         " actions; R1 %02" PRIx32 " %02" PRIx32 " token %02" PRIx32
         " ready %02" PRIx32 " status %02" PRIx32 " %02" PRIx32,
         sector, d.command, enum_name(d.status),
         static_cast<std::uint32_t>(d.completed_actions),
         static_cast<std::uint32_t>(d.response[0]),
         static_cast<std::uint32_t>(d.response[1]),
         static_cast<std::uint32_t>(d.accepted[0]),
         static_cast<std::uint32_t>(d.ready.back()),
         static_cast<std::uint32_t>(d.card_status[0]),
         static_cast<std::uint32_t>(d.card_status[1]));
    }

    daveos::storage::sd::Transport Transport() {
      return {bus_.device<0>(),
              rx_,
              backend_.rate(0),
              card_.sectors,
              {this, [](void* context) {
                 const auto status =
                     static_cast<SdProbe*>(context)->scheduler().yield();
                 return status == core::Status::ok ||
                        status == core::Status::empty ||
                        status == core::Status::depth_limit;
               }}};
    }

    enum class State {
      idle,
      resetting,
      initializing,
      command,
      gap,
      done,
      failed,
      count
    };

    struct Machine
        : core::StateMachine<Machine, State, State::idle,
                             static_cast<std::size_t>(State::count)> {
      void Step(State cs, State& ns, SdProbe& p) {
        switch (cs) {
          case State::idle:
          case State::done:
          case State::failed:
            if (p.reset_requested_) {
              p.reset_requested_ = false;
              p.running_ = true;
              ns = State::resetting;
            } else if (p.requested_) {
              p.requested_ = false;
              p.running_ = true;
              p.backend_.speed(kStartupHz);
              p.command_ = 0;
              p.reads_ = 0;
              p.finished_ = false;
              p.partition_index_ = 0;
              p.lba_ = 0;
              p.error_ = "invalid command response";
              (void)p.initializer_.request();
              ns = State::initializing;
            }
            break;
          case State::resetting:
            if (p.reset_ready_.load(std::memory_order_acquire)) {
              if (p.reset_status_ != hal::Status::ok) {
                p.error_ = enum_name(p.reset_status_);
              }
              p.running_ = false;
              p.ResetReport(p.reset_status_);
              // Reset only repairs SPI; card readiness remains false.
              ns = State::idle;
            }
            break;
          case State::initializing:
            p.initializer_.tick();
            if (const auto result = p.initializer_.result()) {
              p.ocr_ = result->ocr;
              p.command_ = result->command;
              if (result->status == hal::Status::ok) {
                p.command_ = 9;
                ns = State::gap;
              } else {
                p.error_ = enum_name(result->status);
                ns = State::failed;
              }
            }
            break;
          case State::gap:
            if (auto result = p.completion_.result()) {
              if (result->status == hal::Status::ok) {
                ns = State::command;
              } else {
                p.error_ = enum_name(result->status);
                ns = State::failed;
              }
            }
            break;
          case State::command:
            if (auto result = p.completion_.result()) {
              if (result->status != hal::Status::ok) {
                p.error_ = enum_name(result->status);
                ns = State::failed;
                break;
              }
              ns = State::gap;
              switch (p.command_) {
                case 9:
                case 10:
                case 17:
                  if (!p.ReadData()) {
                    ns = State::failed;
                  } else if (p.finished_) {
                    ns = State::done;
                  }
                  break;
                default:
                  ns = State::failed;
                  break;
              }
            }
            break;
          case State::count:
            break;
        }
      }

      void OnEnter(State state, SdProbe& p) {
        if (state == State::resetting) {
          p.reset_ready_.store(false, std::memory_order_release);
          const auto status = p.bus_.reset(
              hal::Callback<hal::ResetResult>::bind<&SdProbe::ResetDone>(p));
          if (status != hal::Status::ok) {
            p.ResetDone({status});
          }
        } else if (state == State::gap) {
          p.actions_[0] = spi::idle_clocks(8);
          (void)p.completion_.start(p.bus_.device<0>(),
                                    std::span{p.actions_}.first(1),
                                    kTransferTimeout);
        } else if (state == State::command) {
          p.tx_ = daveos::storage::sd::command(p.command_,
                                               p.command_ == 17 ? p.lba_ : 0);
          p.rx_.fill(0xff);
          p.receive_size_ = (p.command_ == 17 ? kSectorSize : 16) + 4;
          p.actions_ = daveos::storage::sd::read_actions(
              p.tx_, std::span{p.rx_}.first(p.receive_size_));
          (void)p.completion_.start(p.bus_.device<0>(), p.actions_,
                                    kTransferTimeout);
        } else if (state == State::done || state == State::failed) {
          p.running_ = false;
          p.Report(state == State::done);
        }
      }
    };

    void ResetDone(const hal::ResetResult& result) {
      reset_status_ = result.status;
      reset_ready_.store(true, std::memory_order_release);
    }

    void ResetReport(hal::Status status) {
      I_("SPI1 reset: %s; SD requires sd probe", enum_name(status));
    }

    void Report(bool success) {
      ready_ = success;
      if (success) {
        error_ = "ok";
        I_("SD inspection complete: CRC verified, repeated reads match "
           "(read-only)");
        I_("SD ready: SPI1 %" PRIu32 " Hz, OCR 0x%08" PRIx32
           " (read-only probe)",
           backend_.rate(0), ocr_);
      } else {
        if (const auto init = initializer_.result();
            init && init->status != hal::Status::ok) {
          E_("SD initialization failed at CMD%" PRIu32 ": %s", init->command,
             enum_name(init->status));
          return;
        }
        const auto result = completion_.result();
        E_("SD probe failed at CMD%" PRIu32 ": %s (%s); first bytes %02" PRIx32
           " %02" PRIx32 " %02" PRIx32 " %02" PRIx32,
           command_, error_,
           result ? enum_name(result->status) : "no completion",
           static_cast<std::uint32_t>(rx_[0]),
           static_cast<std::uint32_t>(rx_[1]),
           static_cast<std::uint32_t>(rx_[2]),
           static_cast<std::uint32_t>(rx_[3]));
      }
    }

    bool ReadData() {
      const auto data = sd::data(std::span{rx_}.first(receive_size_),
                                 command_ == 17 ? kSectorSize : 16, 1);
      if (!data) {
        error_ = "missing/error data token, truncated data or CRC mismatch";
        return false;
      }
      if (command_ == 9) {
        auto card = sd::card(*data);
        if (!card) {
          error_ = "unsupported or invalid CSD";
          return false;
        }
        card_ = *card;
        I_("SD capacity: %" PRIu32 " MiB, sectors 0x%08" PRIx32 "%08" PRIx32
           ", maximum clock %" PRIu32 " Hz",
           static_cast<std::uint32_t>(card_.sectors / 2048),
           static_cast<std::uint32_t>(card_.sectors >> 32),
           static_cast<std::uint32_t>(card_.sectors), card_.maximum_hz);
        command_ = 10;
      } else if (command_ == 10) {
        const auto cid = *data;
        // Sanitize untrusted card text before placing it on the console.
        std::array<char, 6> product{};
        std::array<char, 3> oem{};
        for (std::size_t i = 0; i < 5; ++i) {
          product[i] = Printable(cid[i + 3]);
        }
        oem[0] = Printable(cid[1]);
        oem[1] = Printable(cid[2]);
        const auto serial = (std::uint32_t{cid[9]} << 24) |
                            (std::uint32_t{cid[10]} << 16) |
                            (std::uint32_t{cid[11]} << 8) | cid[12];
        I_("SD CID: manufacturer 0x%02" PRIx32
           ", OEM %s, product %s, revision %" PRIu32 ".%" PRIu32
           ", serial %08" PRIx32 ", date %" PRIu32 "-%02" PRIu32,
           static_cast<std::uint32_t>(cid[0]), oem.data(), product.data(),
           static_cast<std::uint32_t>(cid[8] >> 4),
           static_cast<std::uint32_t>(cid[8] & 15), serial,
           2000u + ((std::uint32_t{cid[13] & 15u} << 4) | (cid[14] >> 4)),
           static_cast<std::uint32_t>(cid[14] & 15));
        command_ = 17;
      } else {
        if (reads_ == 0) {
          std::copy(data->begin(), data->end(), sector_.begin());
        } else if (!std::equal(data->begin(), data->end(), sector_.begin())) {
          error_ = "sector changed between repeated reads";
          return false;
        }
        ++reads_;
        if (reads_ == kReadRepeats) {
          I_("SD LBA %" PRIu32 ": %" PRIu32 " CRC-checked reads at %" PRIu32
             " Hz match",
             lba_, kReadRepeats, backend_.rate(0));
          backend_.speed(std::min(kReadHz, card_.maximum_hz));
        } else if (reads_ == 2 * kReadRepeats) {
          I_("SD LBA %" PRIu32 ": %" PRIu32 " CRC-checked reads at %" PRIu32
             " Hz match slow baseline",
             lba_, kReadRepeats, backend_.rate(0));
          Inspect();
        }
      }
      return true;
    }

    static char Printable(std::uint8_t c) {
      return c >= 32 && c <= 126 ? static_cast<char>(c) : '?';
    }

    void ReportVolume(std::uint64_t available) {
      if (auto fs = sd::fat(sector_, available)) {
        I_("SD LBA %" PRIu32 ": %s BPB, %" PRIu32 " sectors, %" PRIu32
           " bytes/cluster, %" PRIu32 " clusters (not mounted)",
           lba_, fs->name, fs->sectors, fs->cluster_bytes, fs->clusters);
      } else if (sd::exfat(sector_)) {
        I_("SD LBA %" PRIu32 ": exFAT signature (not validated or mounted)",
           lba_);
      } else {
        I_("SD LBA %" PRIu32 ": unknown/unsupported filesystem or invalid BPB",
           lba_);
      }
    }

    void Inspect() {
      if (lba_ == 0) {
        if (sd::fat(sector_, card_.sectors) || sd::exfat(sector_)) {
          I_("SD layout: filesystem directly in sector 0 (no partition table)");
          ReportVolume(card_.sectors);
          finished_ = true;
          return;
        }
        auto partitions = sd::partitions(sector_, card_.sectors);
        if (!partitions) {
          I_("SD layout: no valid primary MBR or recognized filesystem boot "
             "sector");
          finished_ = true;
          return;
        }
        partitions_ = *partitions;
        for (std::size_t i = 0; i < partitions_.size(); ++i) {
          const auto& part = partitions_[i];
          if (part.type) {
            I_("SD partition %" PRIu32 ": type 0x%02" PRIx32 ", LBA %" PRIu32
               ", %" PRIu32 " sectors",
               static_cast<std::uint32_t>(i + 1),
               static_cast<std::uint32_t>(part.type), part.start, part.sectors);
          }
        }
      } else {
        ReportVolume(partitions_[partition_index_ - 1].sectors);
      }
      while (partition_index_ < partitions_.size()) {
        const auto& part = partitions_[partition_index_++];
        if (!part.type) {
          continue;
        }
        if (part.type == 0xee || part.type == 5 || part.type == 0x0f ||
            part.type == 0x85) {
          I_("SD GPT/extended partition traversal is not implemented");
          continue;
        }
        lba_ = part.start;
        reads_ = 0;
        backend_.speed(kStartupHz);
        return;
      }
      finished_ = true;
    }

    static hal::Status Prepare(void*) {
      // CKPER uses HSI (64 MHz); /256 yields 250 kHz for SD startup.
      __HAL_RCC_GPIOA_CLK_ENABLE();
      __HAL_RCC_GPIOB_CLK_ENABLE();
      __HAL_RCC_GPIOG_CLK_ENABLE();
      __HAL_RCC_GPIOD_CLK_ENABLE();
      __HAL_RCC_CLKP_CONFIG(RCC_CLKPSOURCE_HSI);
      __HAL_RCC_SPI1_CONFIG(RCC_SPI1CLKSOURCE_CLKP);
      __HAL_RCC_SPI1_CLK_ENABLE();
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
      GPIO_InitTypeDef gpio{};
      gpio.Pin = GPIO_PIN_14;
      gpio.Mode = GPIO_MODE_OUTPUT_PP;
      gpio.Pull = GPIO_NOPULL;
      gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
      HAL_GPIO_Init(GPIOD, &gpio);
      gpio.Mode = GPIO_MODE_AF_PP;
      gpio.Pull = GPIO_PULLUP;
      gpio.Alternate = GPIO_AF5_SPI1;
      gpio.Pin = GPIO_PIN_5;
      HAL_GPIO_Init(GPIOA, &gpio);
      HAL_GPIO_Init(GPIOB, &gpio);
      board::PrepareSdMiso(gpio);
      return hal::Status::ok;
    }

    static void Reset(void*) {
      __HAL_RCC_SPI1_FORCE_RESET();
      __DSB();
      __HAL_RCC_SPI1_RELEASE_RESET();
      __DSB();
    }

    Clock clock_;
    board::SdBusCritical critical_;
    std::uint32_t interrupts_ = 0;
    board::SdDma dma_;
    SdBus backend_;
    Bus bus_;
    daveos::storage::sd::Initializer initializer_;
    spi::Completion completion_;
    std::atomic<bool> reset_ready_{false};
    hal::Status reset_status_ = hal::Status::ok;
    Machine machine_;
    std::array<std::uint8_t, 6> tx_{};
    std::array<std::uint8_t, kReceiveCapacity> rx_{};
    std::array<std::uint8_t, kSectorSize> sector_{};
    std::array<sd::Partition, 4> partitions_{};
    sd::Card card_{};
    std::size_t receive_size_ = 0, partition_index_ = 0;
    std::uint32_t lba_ = 0, reads_ = 0;
    const char* error_ = "";
    bool finished_ = false;
    std::array<spi::Action, 6> actions_{};
    std::uint32_t command_ = 0, ocr_ = 0;
    bool requested_ = false, running_ = false, ready_ = false, leased_ = false;
    bool reset_requested_ = false;
  };


}  // namespace app
