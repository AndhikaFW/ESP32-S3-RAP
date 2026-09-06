#include "luckfox_spi.h"

#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

// Wire protocol (LuckFox/Linux spidev is master, this is the slave -- see
// config.h for why): every SPI transaction is exactly kLuckfoxSpiChunkBytes
// long (spi_slave needs a same-size buffer queued before the master clocks
// anything in, so a fixed transaction size keeps both ends trivially in
// sync without a separate length negotiation step).
//
// The first transaction of a frame is a header, packed into the front of
// the chunk buffer (rest zero-padded/ignored):
//   uint32_t magic       -- kMagic, lets the slave resync if it ever misses
//                            a transaction boundary (e.g. after a reset)
//   uint8_t  stream_id
//   uint16_t seq
//   uint32_t data_len     -- total payload bytes across all following chunks
// Every following transaction until data_len bytes have been collected is a
// raw payload chunk; the last one is only partially used (data_len mod
// kLuckfoxSpiChunkBytes bytes of it), the rest is padding and discarded.
//
// See luckfox/spi_sender.py in the parent repo (LuckFox side, Python +
// spidev, outside this firmware submodule) for the sender half of this
// same protocol.
namespace luckfox_spi {

namespace {

constexpr const char *kTag = "luckfox_spi";
constexpr uint32_t kMagic = 0x52415046;  // "RAPF"

struct __attribute__((packed)) WireHeader {
  uint32_t magic;
  uint8_t stream_id;
  uint16_t seq;
  uint32_t data_len;
};

FrameCallback g_onFrame;

// Reused DMA-capable scratch buffers for the SPI transaction itself -- the
// slave driver requires tx/rx buffers to be DMA-capable memory, which PSRAM
// frame buffers aren't guaranteed to be, so every chunk gets memcpy'd out of
// this scratch into the real (PSRAM) frame buffer after the transaction
// completes.
uint8_t *g_rxScratch = nullptr;
uint8_t *g_txScratch = nullptr;  // slave has nothing to say back; stays zeroed

bool transactOnce() {
  spi_slave_transaction_t t{};
  t.length = kLuckfoxSpiChunkBytes * 8;  // bits
  t.tx_buffer = g_txScratch;
  t.rx_buffer = g_rxScratch;
  esp_err_t err = spi_slave_transmit(kLuckfoxSpiHost, &t, portMAX_DELAY);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "spi_slave_transmit failed: %d", err);
    return false;
  }
  return true;
}

void receiverTask(void * /*arg*/) {
  uint32_t diagCount = 0;
  while (true) {
    // -- Wait for a valid header transaction --
    WireHeader hdr{};
    while (true) {
      if (!transactOnce()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      memcpy(&hdr, g_rxScratch, sizeof(hdr));
      if (hdr.magic == kMagic) break;
      // Not a header (or we're out of sync with the sender) -- keep
      // consuming transactions until one lines up. LuckFox retries the
      // whole frame on timeout (see luckfox/spi_sender.py), so this
      // self-heals.
      //
      // DIAGNOSTIC (temporary, see project notes on the SPI bring-up):
      // log every 200th non-matching transaction's first bytes so we can
      // tell "receiving nothing" (all zero) apart from "receiving garbage
      // out of sync" (non-zero, wrong magic) from the serial capture.
      if ((diagCount++ % 200) == 0) {
        ESP_LOGI(kTag, "no-sync #%u: %02x %02x %02x %02x %02x %02x %02x %02x", (unsigned)diagCount,
                 g_rxScratch[0], g_rxScratch[1], g_rxScratch[2], g_rxScratch[3], g_rxScratch[4],
                 g_rxScratch[5], g_rxScratch[6], g_rxScratch[7]);
      }
    }

    if (hdr.data_len == 0 || hdr.data_len > kVideoMaxFrameBytes) {
      ESP_LOGW(kTag, "bad frame header (len=%u), resyncing", hdr.data_len);
      continue;
    }

    auto *data = static_cast<uint8_t *>(heap_caps_malloc(hdr.data_len, MALLOC_CAP_SPIRAM));
    if (data == nullptr) {
      data = static_cast<uint8_t *>(heap_caps_malloc(hdr.data_len, MALLOC_CAP_8BIT));
    }
    if (data == nullptr) {
      ESP_LOGW(kTag, "frame alloc failed (%u bytes), dropping frame", hdr.data_len);
      // Still have to consume the chunks the sender is about to clock in,
      // otherwise the next header transaction lands mid-payload.
      uint32_t remaining = hdr.data_len;
      while (remaining > 0) {
        if (!transactOnce()) break;
        remaining -= remaining < kLuckfoxSpiChunkBytes ? remaining : kLuckfoxSpiChunkBytes;
      }
      continue;
    }

    // -- Collect chunks --
    uint32_t received = 0;
    bool ok = true;
    while (received < hdr.data_len) {
      if (!transactOnce()) {
        ok = false;
        break;
      }
      uint32_t want = hdr.data_len - received;
      if (want > kLuckfoxSpiChunkBytes) want = kLuckfoxSpiChunkBytes;
      memcpy(data + received, g_rxScratch, want);
      received += want;
    }

    if (!ok) {
      heap_caps_free(data);
      ESP_LOGW(kTag, "frame incomplete (got %u/%u), dropping", received, hdr.data_len);
      continue;
    }

    ESP_LOGI(kTag, "frame received: stream=%u seq=%u len=%u", hdr.stream_id, hdr.seq, hdr.data_len);
    if (g_onFrame) {
      g_onFrame(hdr.stream_id, hdr.seq, data, hdr.data_len);  // callback takes ownership
    } else {
      heap_caps_free(data);
    }
  }
}

}  // namespace

void init(FrameCallback onFrame) {
  g_onFrame = std::move(onFrame);

  g_rxScratch = static_cast<uint8_t *>(heap_caps_malloc(kLuckfoxSpiChunkBytes, MALLOC_CAP_DMA));
  g_txScratch = static_cast<uint8_t *>(heap_caps_malloc(kLuckfoxSpiChunkBytes, MALLOC_CAP_DMA));
  if (g_rxScratch == nullptr || g_txScratch == nullptr) {
    ESP_LOGE(kTag, "failed to allocate DMA scratch buffers");
    return;
  }
  memset(g_txScratch, 0, kLuckfoxSpiChunkBytes);

  spi_bus_config_t busCfg{};
  busCfg.mosi_io_num = kLuckfoxSpiMosiPin;
  busCfg.miso_io_num = kLuckfoxSpiMisoPin;
  busCfg.sclk_io_num = kLuckfoxSpiSckPin;
  busCfg.quadwp_io_num = -1;
  busCfg.quadhd_io_num = -1;

  spi_slave_interface_config_t slaveCfg{};
  slaveCfg.mode = 0;
  slaveCfg.spics_io_num = kLuckfoxSpiCsPin;
  slaveCfg.queue_size = 3;
  slaveCfg.flags = 0;

  esp_err_t err = spi_slave_initialize(kLuckfoxSpiHost, &busCfg, &slaveCfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "spi_slave_initialize failed: %d", err);
    return;
  }

  // Pull-ups on the SPI lines -- without them, CS/CLK/MOSI float between
  // transactions (LuckFox and this ESP32 are on separate USB power rails,
  // no external pull network), and the slave peripheral's edge-detection can
  // apparently miss/misread transaction boundaries on noise even though
  // every line tests out fine electrically otherwise. Matches ESP-IDF's own
  // spi_slave/receiver example, which calls this out explicitly.
  gpio_set_pull_mode(static_cast<gpio_num_t>(kLuckfoxSpiMosiPin), GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(kLuckfoxSpiSckPin), GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(kLuckfoxSpiCsPin), GPIO_PULLUP_ONLY);

  xTaskCreate(receiverTask, "luckfox_spi_rx", 4096, nullptr, 5, nullptr);
  ESP_LOGI(kTag, "SPI slave up (host=%d, chunk=%u bytes)", kLuckfoxSpiHost, (unsigned)kLuckfoxSpiChunkBytes);
}

}  // namespace luckfox_spi
