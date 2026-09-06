#pragma once

#include <cstdint>
#include <functional>

// SPI slave link to this node's paired LuckFox (see config.h for pins/host
// and the wire protocol comment in luckfox_spi.cpp). LuckFox's Linux side
// is the SPI master; this only ever receives.
namespace luckfox_spi {

// Called on the SPI-receiver task's own context once a complete frame has
// arrived (header + all chunks). `data` is heap_caps_malloc'd (PSRAM if
// available) and ownership passes to the callback -- it must either free it
// (heap_caps_free) or hand it off to something that will (e.g. video_relay's
// frame queue, which already owns/frees Frame::data the same way).
using FrameCallback = std::function<void(uint8_t stream_id, uint16_t seq, uint8_t *data, uint32_t len)>;

void init(FrameCallback onFrame);

}  // namespace luckfox_spi
