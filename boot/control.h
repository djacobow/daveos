#pragma once

#include "journal.h"
#include "util/crc32.h"

namespace daveos::boot {


  struct Selection {
    Status status = Status::not_found;
    std::uint32_t slot = 2;
    bool trial = false;
    Image image{};
  };

  // Portable boot policy. The platform validates vectors and performs the CPU
  // handoff/reset; this object never jumps or accesses registers. Confirmation
  // callers must identify their actual executing slot (platform VTOR on STM32).
  class Control {
   public:
    Control(Flash flash, const Layout& layout,
            const util::crc32::Service& crc = util::crc32::Service{})
        : flash_(flash), layout_(layout), journal_(flash, layout), crc_(crc) {}

    Selection select();
    Status confirm_image(std::uint32_t running_slot);
    Status verify(std::uint32_t slot, const Image& image) const;

    Status status(Snapshot& snapshot) { return journal_.load(snapshot); }

   private:
    Flash flash_;
    Layout layout_;
    Journal journal_;
    util::crc32::Service crc_;
  };


}  // namespace daveos::boot
