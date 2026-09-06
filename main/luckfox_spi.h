#pragma once

#include <cstdint>
#include <functional>

// SPI slave link to this node's paired LuckFox (see config.h for pins/host
// and the wire protocol comment in luckfox_spi.cpp). LuckFox's Linux side
// is the SPI master; this only ever receives.
namespace luckfox_spi {

// Called on the SPI-receiver task's own context once a complete video frame
// or plate crop has arrived (header + all chunks) -- both are images, so
// they share this callback; a plate crop for lane N is distinguished by
// stream_id == kPlateStreamBase+N (see config.h) instead of a real video
// lane's 0..kVideoStreamsPerNode-1. `data` is heap_caps_malloc'd (PSRAM if
// available) and ownership passes to the callback -- it must either free it
// (heap_caps_free) or hand it off to something that will (e.g. video_relay's
// frame queue, which already owns/frees Frame::data the same way).
using FrameCallback = std::function<void(uint8_t stream_id, uint16_t seq, uint8_t *data, uint32_t len)>;

// Called on the same task once a lane status reading has arrived (motion ->
// parked-car check -> occupancy, see luckfox/parking_detector.py). `plate`
// is only ever "" for now -- see the comment on StatusPacket in protocol.h
// for why LuckFox can't OCR it itself. Points at a stack buffer valid only
// for the duration of the call.
using StatusCallback = std::function<void(uint8_t stream_id, uint8_t occupied, const char *plate)>;

void init(FrameCallback onFrame, StatusCallback onStatus);

}  // namespace luckfox_spi
