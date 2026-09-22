#include "control.h"

#include <algorithm>

namespace daveos::boot {


  Status Control::verify(std::uint32_t slot, const Image& image) const {
    if (!flash_.valid() || !layout_.valid() || slot >= 2 || !image.size ||
        image.size > layout_.slot_size || image.product != layout_.product ||
        image.revision != layout_.revision || !image.installation) {
      return Status::incompatible;
    }
    std::array<std::byte, 256> buffer;
    std::uint32_t crc = 0;
    for (std::uint32_t offset = 0; offset < image.size;) {
      auto count = std::min<std::size_t>(buffer.size(), image.size - offset);
      auto bytes = std::span(buffer).first(count);
      auto result =
          flash_.read(flash_.context, layout_.slots[slot] + offset, bytes);
      if (result != Status::ok) {
        return result;
      }
      crc = crc_.update(crc, bytes);
      offset += count;
    }
    return crc == image.crc ? Status::ok : Status::checksum_error;
  }

  Selection Control::select() {
    Snapshot snapshot;
    auto result = journal_.load(snapshot);
    if (result != Status::ok) {
      return {result};
    }
    bool rejected = false;
    for (auto& image : snapshot.images) {
      if (image.state == ImageState::trial) {
        image.state = ImageState::rejected;
        rejected = true;
      }
    }
    if (rejected) {
      result = journal_.commit(snapshot);
      if (result != Status::ok) {
        return {result};
      }
    }
    const std::uint32_t newest =
        snapshot.images[1].installation > snapshot.images[0].installation ? 1
                                                                          : 0;
    Status failure = Status::not_found;
    for (auto slot : {newest, 1 - newest}) {
      auto& image = snapshot.images[slot];
      if (image.state != ImageState::pending &&
          image.state != ImageState::confirmed) {
        continue;
      }
      result = verify(slot, image);
      if (result != Status::ok) {
        failure = result;
        continue;
      }
      const bool trial = image.state == ImageState::pending;
      if (trial) {
        image.state = ImageState::trial;
        result = journal_.commit(snapshot);
        if (result != Status::ok) {
          return {result};
        }
      }
      return {Status::ok, slot, trial, image};
    }
    return {failure};
  }

  Status Control::confirm_image(std::uint32_t running_slot) {
    if (running_slot >= 2) {
      return Status::unsupported;
    }
    Snapshot snapshot;
    auto result = journal_.load(snapshot);
    if (result != Status::ok) {
      return result;
    }
    auto& image = snapshot.images[running_slot];
    if (image.state == ImageState::confirmed) {
      return Status::ok;
    }
    if (image.state != ImageState::trial) {
      return Status::not_running;
    }
    image.state = ImageState::confirmed;
    return journal_.commit(snapshot);
  }


}  // namespace daveos::boot
