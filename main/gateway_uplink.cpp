#include "gateway_uplink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_enc28j60.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "config.h"

namespace gateway_uplink {

namespace {

constexpr const char *kTag = "gateway_uplink";

esp_eth_handle_t g_ethHandle = nullptr;
esp_netif_t *g_netif = nullptr;
volatile bool g_linkUp = false;

// One video frame queued for the Ethernet uplink; owns `data` until it's
// either sent or dropped (see freeVideoItem()).
struct VideoItem {
  uint8_t origin_node_id;
  uint8_t stream_id;
  uint16_t seq;
  uint32_t data_len;
  uint8_t *data;
};

// One UDP datagram's worth of one video frame -- see kVideoUdpChunkBytes's
// comment in config.h for why chunked-over-UDP instead of one TCP stream.
// chunk_count is repeated on every chunk (not just chunk 0) since UDP
// datagrams can arrive out of order or chunk 0 can simply be the one that's
// lost -- the backend needs to know the total from whichever chunks
// actually show up (see backend/video_listener.py's reassembly).
struct __attribute__((packed)) VideoChunkHeader {
  uint8_t origin_node_id;
  uint8_t stream_id;  // which of this node's kVideoStreamsPerNode cameras
  uint16_t seq;        // which frame
  uint16_t chunk_index;
  uint16_t chunk_count;
};

QueueHandle_t g_videoQueue = nullptr;

void freeVideoItem(VideoItem *item) {
  if (item == nullptr) return;
  if (item->data != nullptr) heap_caps_free(item->data);
  delete item;
}

// Drains g_videoQueue over a UDP socket, one connect() at task start (UDP
// has no connection to lose, so unlike the old TCP version there's nothing
// to reconnect -- connect() on a datagram socket just fixes the default
// destination for send() and filters unrelated incoming packets, it
// doesn't establish a session). Splits each frame into
// ceil(data_len / kVideoUdpChunkBytes) datagrams; a send() failure just
// drops the rest of that one frame's chunks (logged, not retried -- see
// kVideoBackendPort's comment in config.h for why that's an acceptable
// trade for this link).
void videoUplinkTask(void * /*arg*/) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(kTag, "video uplink socket() failed, video will never be sent");
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(kVideoBackendPort);
  inet_pton(AF_INET, kBackendHost, &dest.sin_addr);
  // Just fixes this UDP socket's default destination for send() below --
  // unlike TCP, no handshake happens, so a bad kBackendHost is the only
  // realistic failure mode here (and that's a config bug, not a runtime
  // condition to retry on).
  if (connect(sock, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) != 0) {
    ESP_LOGE(kTag, "video uplink connect() failed, video will never be sent");
    vTaskDelete(nullptr);
    return;
  }

  uint8_t chunkBuf[sizeof(VideoChunkHeader) + kVideoUdpChunkBytes];
  auto *chunkHdr = reinterpret_cast<VideoChunkHeader *>(chunkBuf);
  uint8_t *chunkPayload = chunkBuf + sizeof(VideoChunkHeader);

  while (true) {
    VideoItem *item = nullptr;
    if (xQueueReceive(g_videoQueue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
      continue;
    }
    if (!g_linkUp) {
      freeVideoItem(item);
      continue;
    }

    uint16_t chunkCount = static_cast<uint16_t>(
        (item->data_len + kVideoUdpChunkBytes - 1) / kVideoUdpChunkBytes);
    if (chunkCount == 0) chunkCount = 1;  // still send one (empty) chunk for a zero-length frame

    chunkHdr->origin_node_id = item->origin_node_id;
    chunkHdr->stream_id = item->stream_id;
    chunkHdr->seq = item->seq;
    chunkHdr->chunk_count = chunkCount;

    for (uint16_t i = 0; i < chunkCount; i++) {
      size_t offset = static_cast<size_t>(i) * kVideoUdpChunkBytes;
      size_t remaining = static_cast<size_t>(item->data_len) - offset;
      size_t len = std::min(kVideoUdpChunkBytes, remaining);
      chunkHdr->chunk_index = i;
      memcpy(chunkPayload, item->data + offset, len);
      if (send(sock, chunkBuf, sizeof(VideoChunkHeader) + len, 0) < 0) {
        ESP_LOGW(kTag, "video chunk send failed, dropping rest of node=%u stream=%u seq=%u",
                 item->origin_node_id, item->stream_id, item->seq);
        break;
      }
    }
    freeVideoItem(item);
  }
}

void ethEventHandler(void *, esp_event_base_t, int32_t eventId, void *eventData) {
  auto handle = *static_cast<esp_eth_handle_t *>(eventData);
  uint8_t mac[6] = {0};
  switch (eventId) {
    case ETHERNET_EVENT_CONNECTED:
      g_linkUp = true;
      esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
      ESP_LOGI(kTag, "link up, MAC=%02x:%02x:%02x:%02x:%02x:%02x, static ip=%s", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5], kEthStaticIp);
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      g_linkUp = false;
      ESP_LOGW(kTag, "link down");
      break;
    default:
      break;
  }
}

}  // namespace

bool init() {
  // Created unconditionally, before anything ENC28J60-specific that can
  // fail below: flushVideo() must stay safe to call (it just drops frames
  // while g_linkUp is false) even if the Ethernet driver never comes up.
  g_videoQueue = xQueueCreate(kVideoUplinkQueueDepth, sizeof(VideoItem *));
  // 6144, not the old TCP version's 4096 -- videoUplinkTask now keeps a
  // sizeof(VideoChunkHeader)+kVideoUdpChunkBytes (~1.4KB) scratch buffer on
  // its own stack frame for building each outgoing chunk, on top of the
  // usual FreeRTOS/lwIP call-chain overhead the old 4096 was sized for.
  xTaskCreate(videoUplinkTask, "video_uplink", 6144, nullptr, 4, nullptr);

  // Shared ISR service the enc28j60 driver needs for its interrupt GPIO;
  // ESP_ERR_INVALID_STATE just means something else already installed it.
  esp_err_t isrErr = gpio_install_isr_service(0);
  if (isrErr != ESP_OK && isrErr != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "gpio_install_isr_service failed: %d", isrErr);
    return false;
  }

  spi_bus_config_t busCfg{};
  busCfg.mosi_io_num = kEncMosiPin;
  busCfg.miso_io_num = kEncMisoPin;
  busCfg.sclk_io_num = kEncSckPin;
  busCfg.quadwp_io_num = -1;
  busCfg.quadhd_io_num = -1;
  if (spi_bus_initialize(kEncSpiHost, &busCfg, SPI_DMA_CH_AUTO) != ESP_OK) {
    ESP_LOGE(kTag, "spi_bus_initialize failed");
    return false;
  }

  spi_device_interface_config_t spiDevCfg{};
  spiDevCfg.mode = 0;
  spiDevCfg.clock_speed_hz = kEncSpiClockMhz * 1000 * 1000;
  spiDevCfg.queue_size = 20;
  spiDevCfg.spics_io_num = kEncCsPin;
  spiDevCfg.cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(kEncSpiClockMhz);

  eth_enc28j60_config_t encConfig = ETH_ENC28J60_DEFAULT_CONFIG(kEncSpiHost, &spiDevCfg);
  encConfig.int_gpio_num = kEncIntPin;

  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  esp_eth_mac_t *mac = esp_eth_mac_new_enc28j60(&encConfig, &macConfig);
  if (mac == nullptr) {
    ESP_LOGE(kTag, "creating ENC28J60 MAC instance failed");
    return false;
  }

  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.autonego_timeout_ms = 0;  // ENC28J60 doesn't support auto-negotiation
  phyConfig.reset_gpio_num = -1;      // ENC28J60 has no PHY reset pin
  esp_eth_phy_t *phy = esp_eth_phy_new_enc28j60(&phyConfig);
  if (phy == nullptr) {
    ESP_LOGE(kTag, "creating ENC28J60 PHY instance failed");
    mac->del(mac);
    return false;
  }

  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
  if (esp_eth_driver_install(&ethConfig, &g_ethHandle) != ESP_OK) {
    ESP_LOGE(kTag, "esp_eth_driver_install failed (no/faulty ENC28J60 on the bus?)");
    // A failed install can still leave the MAC's interrupt handler attached
    // to kEncIntPin; without this, a floating INT pin (no chip wired up)
    // keeps firing into a half-initialized driver and eventually crashes.
    mac->del(mac);
    phy->del(phy);
    return false;
  }

  // ENC28J60 Errata #1: silicon revisions below B5 need >=8MHz SPI clock.
  // Checked here (not before esp_eth_driver_install above) because
  // emac_enc28j60_get_chip_info() just returns a struct field that's only
  // populated by the real SPI chip-ID read inside init() -- calling it any
  // earlier always sees the zero-initialized default, so the check silently
  // never passed regardless of the actual chip's revision (caught when
  // dropping kEncSpiClockMhz below 8 failed this on a confirmed-B7 chip).
  if (emac_enc28j60_get_chip_info(mac) < ENC28J60_REV_B5 && kEncSpiClockMhz < 8) {
    ESP_LOGE(kTag, "SPI clock must be >=8MHz for this ENC28J60 silicon revision");
    esp_eth_driver_uninstall(g_ethHandle);
    return false;
  }

  // ENC28J60 has no burned-in MAC; it must be set before any traffic.
  esp_eth_ioctl(g_ethHandle, ETH_CMD_S_MAC_ADDR, const_cast<uint8_t *>(kEncMac));
  eth_duplex_t duplex = ETH_DUPLEX_FULL;
  esp_eth_ioctl(g_ethHandle, ETH_CMD_S_DUPLEX_MODE, &duplex);

  esp_netif_config_t netifCfg = ESP_NETIF_DEFAULT_ETH();
  g_netif = esp_netif_new(&netifCfg);
  esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(g_ethHandle);
  if (esp_netif_attach(g_netif, glue) != ESP_OK) {
    ESP_LOGE(kTag, "esp_netif_attach failed");
    return false;
  }

  // Static IP: this is a direct cable to RPi4's eth0, no DHCP server on the
  // link (see config.h kEthStatic*). ESP_NETIF_DEFAULT_ETH() starts with a
  // DHCP client enabled, so that has to be stopped before assigning one.
  esp_netif_dhcpc_stop(g_netif);
  esp_netif_ip_info_t ipInfo{};
  ipInfo.ip.addr = ipaddr_addr(kEthStaticIp);
  ipInfo.gw.addr = ipaddr_addr(kEthStaticGateway);
  ipInfo.netmask.addr = ipaddr_addr(kEthStaticNetmask);
  esp_netif_set_ip_info(g_netif, &ipInfo);

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler, nullptr);

  if (esp_eth_start(g_ethHandle) != ESP_OK) {
    ESP_LOGE(kTag, "esp_eth_start failed");
    return false;
  }
  return true;
}

void flush(uint8_t nodeId, uint8_t streamId, uint8_t occupied, const char *plate) {
  if (!g_linkUp) {
    ESP_LOGW(kTag, "link down, dropping reading from node %u stream %u", nodeId, streamId);
    return;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGW(kTag, "socket() failed, dropping reading from node %u stream %u", nodeId, streamId);
    return;
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(kBackendPort);
  inet_pton(AF_INET, kBackendHost, &dest.sin_addr);

  // 2s connect/send budget so a dead backend never stalls the chain.
  timeval tv{};
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (connect(sock, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) != 0) {
    ESP_LOGW(kTag, "connect to %s:%u failed, dropping reading from node %u stream %u", kBackendHost,
              kBackendPort, nodeId, streamId);
    close(sock);
    return;
  }

  // Compact line-oriented payload: "node:stream:occupied:plate\n" -- one
  // line per reading, sent as soon as it arrives (no more batching by lap).
  // Swap this for whatever framing the ParkingVision backend expects once
  // that API is defined -- video/plate-crop images never flow through here,
  // only this small per-lane text summary.
  char line[20 + kMaxPlateLen];
  int written = snprintf(line, sizeof(line), "%u:%u:%u:%s", nodeId, streamId, occupied, plate);
  if (written < static_cast<int>(sizeof(line))) {
    line[written++] = '\n';
  }

  send(sock, line, written, 0);
  close(sock);
}

void flushVideo(uint8_t originNodeId, uint8_t streamId, uint16_t seq, uint8_t *data, uint32_t dataLen) {
  if (g_videoQueue == nullptr || !g_linkUp) {
    heap_caps_free(data);
    return;
  }
  auto *item = new VideoItem{originNodeId, streamId, seq, dataLen, data};
  if (xQueueSend(g_videoQueue, &item, 0) != pdTRUE) {
    ESP_LOGW(kTag, "video uplink queue full, dropping node=%u stream=%u", originNodeId, streamId);
    freeVideoItem(item);
  }
}

}  // namespace gateway_uplink
