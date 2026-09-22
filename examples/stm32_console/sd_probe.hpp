#pragma once

#include <algorithm>
#include <cinttypes>

#include "application.h"
#include "hal/adapters/daveos.hpp"
#include "hal/controller.hpp"
#include "sd_board.h"
#include "sd_inspect.h"
#include "storage/sd/session.h"

namespace app {


  namespace hal = daveos::hal;
  namespace spi = hal::spi;

  // Nucleo SPI1 wiring for a storage::sd::Session, plus the read-only `sd`
  // diagnostics: after the CSD, report the CID, compare repeated CRC-checked
  // reads at 250 kHz and 1 MHz, and describe the partition layout. The
  // session owns the card protocol and block device; this module owns pins,
  // clocks, DMA and console output. Interrupt callbacks only publish HAL
  // completions.
  class SdProbe : public core::Module<SdProbe, Event> {
    using Clock = hal::DaveOsClock<core::SchedulerInterface<Event>, Platform>;
    using Session = daveos::storage::sd::Session;

    // Dedicated fixture backend: keep a private config copy so startup can
    // use <=400 kHz and completed transactions can switch to a conservative
    // data rate. Only this module owns the bus; the session changes speed
    // between transactions, never while a HAL operation owns the peripheral.
    class SdBus : public board::SdSpiBus {
     public:
      explicit SdBus(const daveos::platform::stm32::detail::SpiDma& dma)
          : board::SdSpiBus(SPI1, SPI1_IRQn, HSI_VALUE,
                            {nullptr, Prepare, Reset, nullptr}, dma) {}

      hal::Status init(std::span<const Config> devices) {
        configs_[0] = devices[0];
        configs_[0].maximum_hz = Session::kStartupHz;
        return board::SdSpiBus::init(configs_);
      }

      void speed(std::uint32_t hz) { configs_[0].maximum_hz = hz; }

     private:
      std::array<Config, 1> configs_{};
    };

    using Bus = hal::Controller<SdBus, Clock, board::SdBusCritical, 1>;
    static constexpr std::uint32_t kStartupHz = Session::kStartupHz;
    static constexpr std::uint32_t kReadHz = Session::kDataHz;
    static constexpr std::uint32_t kReadRepeats = 3;
    static constexpr std::size_t kSectorSize = 512;
    static constexpr auto kPeriod = std::chrono::milliseconds{1};

   public:
    explicit SdProbe(Platform& platform, bool dma = false)
        : clock_(platform),
          dma_(dma),
          backend_(dma_.operations()),
          bus_(backend_, clock_, critical_,
               {{{{GPIOD, GPIO_PIN_14, false}, kReadHz, 0, false}}}),
          session_(bus_.device<0>(), SpeedHooks(), ResetHooks(), PumpHook(),
                   Diagnostics()) {}

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
      return session_.request() == hal::Status::ok ? core::Status::ok
                                                   : core::Status::busy;
    }

    core::Status ResetBus() {
      return session_.reset() == hal::Status::ok ? core::Status::ok
                                                 : core::Status::busy;
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
         bus_.faulted() ? "yes" : "no", session_.ready() ? "yes" : "no",
         session_.error());
      return core::Status::ok;
    }

    // File-scope wiring only takes the session's address; no card access.
    daveos::storage::BlockDevice block_device() {
      return session_.block_device();
    }

    void Tick() { session_.tick(); }

   private:
    Session::Speed SpeedHooks() {
      return {
          this,
          [](void* p, std::uint32_t hz) {
            static_cast<SdProbe*>(p)->backend_.speed(hz);
          },
          [](void* p) { return static_cast<SdProbe*>(p)->backend_.rate(0); }};
    }

    Session::Reset ResetHooks() {
      return {this, [](void* p, hal::Callback<hal::ResetResult> done) {
                return static_cast<SdProbe*>(p)->bus_.reset(done);
              }};
    }

    daveos::storage::sd::Transport::Pump PumpHook() {
      return {this, [](void* context) {
                const auto status =
                    static_cast<SdProbe*>(context)->scheduler().yield();
                return status == core::Status::ok ||
                       status == core::Status::empty ||
                       status == core::Status::depth_limit;
              }};
    }

    Session::Observer Diagnostics() {
      return {this,
              [](void* p) { static_cast<SdProbe*>(p)->Started(); },
              [](void* p, std::uint32_t command,
                 std::span<const std::uint8_t> data) {
                return static_cast<SdProbe*>(p)->Next(command, data);
              },
              [](void* p, bool success) {
                static_cast<SdProbe*>(p)->Report(success);
              },
              [](void* p, hal::Status status) {
                static_cast<SdProbe*>(p)->ResetReport(status);
              },
              [](void* p, std::uint32_t sector) {
                static_cast<SdProbe*>(p)->ReadFailure(sector);
              },
              [](void* p, std::uint32_t sector,
                 const daveos::storage::sd::Transport::WriteDiagnostics& d) {
                static_cast<SdProbe*>(p)->WriteFailure(sector, d);
              }};
    }

    void ReadFailure(std::uint32_t sector) {
      E_("SD read LBA %" PRIu32 ": %s; reinitialization required", sector,
         session_.error());
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

    void Started() {
      reads_ = 0;
      partition_index_ = 0;
      lba_ = 0;
    }

    void ResetReport(hal::Status status) {
      I_("SPI1 reset: %s; SD requires sd probe", enum_name(status));
    }

    void Report(bool success) {
      if (success) {
        I_("SD inspection complete: CRC verified, repeated reads match "
           "(read-only)");
        I_("SD ready: SPI1 %" PRIu32 " Hz, OCR 0x%08" PRIx32
           " (read-only probe)",
           backend_.rate(0), session_.ocr());
      } else {
        if (const auto init = session_.initialization();
            init && init->status != hal::Status::ok) {
          E_("SD initialization failed at CMD%" PRIu32 ": %s", init->command,
             enum_name(init->status));
          return;
        }
        const auto result = session_.last_transfer();
        const auto rx = session_.wire();
        E_("SD probe failed at CMD%" PRIu32 ": %s (%s); first bytes %02" PRIx32
           " %02" PRIx32 " %02" PRIx32 " %02" PRIx32,
           session_.command(), session_.error(),
           result ? enum_name(result->status) : "no completion",
           static_cast<std::uint32_t>(rx[0]), static_cast<std::uint32_t>(rx[1]),
           static_cast<std::uint32_t>(rx[2]),
           static_cast<std::uint32_t>(rx[3]));
      }
    }

    // After the CSD (9), report the CID (10), then run repeated sector reads
    // (17) and the partition walk.
    Session::Step Next(std::uint32_t command,
                       std::span<const std::uint8_t> data) {
      using Kind = Session::Step::Kind;
      if (command == 9) {
        card_ = session_.card();
        I_("SD capacity: %" PRIu32 " MiB, sectors 0x%08" PRIx32 "%08" PRIx32
           ", maximum clock %" PRIu32 " Hz",
           static_cast<std::uint32_t>(card_.sectors / 2048),
           static_cast<std::uint32_t>(card_.sectors >> 32),
           static_cast<std::uint32_t>(card_.sectors), card_.maximum_hz);
        return {Kind::read, 10};
      }
      if (command == 10) {
        const auto cid = data;
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
        return {Kind::read, 17, lba_};
      }
      if (reads_ == 0) {
        std::copy(data.begin(), data.end(), sector_.begin());
      } else if (!std::equal(data.begin(), data.end(), sector_.begin())) {
        return {Kind::fail, 0, 0, "sector changed between repeated reads"};
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
        if (Inspect()) {
          return {Kind::finish};
        }
      }
      return {Kind::read, 17, lba_};
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

    // Returns true when the walk is finished; otherwise lba_ names the next
    // partition to read at the startup rate.
    bool Inspect() {
      if (lba_ == 0) {
        if (sd::fat(sector_, card_.sectors) || sd::exfat(sector_)) {
          I_("SD layout: filesystem directly in sector 0 (no partition table)");
          ReportVolume(card_.sectors);
          return true;
        }
        auto partitions = sd::partitions(sector_, card_.sectors);
        if (!partitions) {
          I_("SD layout: no valid primary MBR or recognized filesystem boot "
             "sector");
          return true;
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
        return false;
      }
      return true;
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
    Session session_;
    std::array<std::uint8_t, kSectorSize> sector_{};
    std::array<sd::Partition, 4> partitions_{};
    sd::Card card_{};
    std::size_t partition_index_ = 0;
    std::uint32_t lba_ = 0, reads_ = 0;
  };


}  // namespace app
