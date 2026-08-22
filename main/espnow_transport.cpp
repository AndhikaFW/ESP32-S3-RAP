#include "espnow_transport.h"

#include <cstring>

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "config.h"

namespace espnow {

namespace {

constexpr const char *kTag = "espnow";
constexpr size_t kMaxPacketBytes = 250;  // ESP-NOW hard limit per frame
constexpr size_t kRxQueueDepth = 16;

struct RxItem {
  uint8_t mac[6];
  uint8_t data[kMaxPacketBytes];
  uint8_t len;
};

QueueHandle_t g_rxQueue = nullptr;
RecvHandler g_handler = nullptr;
const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Registered with esp_now; runs in the WiFi task, so it must stay tiny and
// non-blocking -- it only copies the frame into g_rxQueue for poll() to
// process later on the caller's task.
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len <= 0 || static_cast<size_t>(len) > kMaxPacketBytes || g_rxQueue == nullptr) {
    return;
  }
  RxItem item{};
  memcpy(item.mac, info->src_addr, 6);
  memcpy(item.data, data, len);
  item.len = static_cast<uint8_t>(len);
  xQueueSend(g_rxQueue, &item, 0);
}

}  // namespace

bool init() {
  wifi_init_config_t wifiCfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&wifiCfg) != ESP_OK) {
    return false;
  }
  // ESP-NOW doesn't need calibration data persisted across reboots, and
  // skipping NVS storage here avoids fighting our own provisioning writes.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);

  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
    return false;
  }
  // No AP to negotiate a channel with, so every ring node fixes the same one.
  esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    return false;
  }

  g_rxQueue = xQueueCreate(kRxQueueDepth, sizeof(RxItem));
  if (g_rxQueue == nullptr) {
    return false;
  }

  esp_now_register_recv_cb(onDataRecv);

  // Broadcast is a peer like any other as far as esp_now_send is concerned.
  addPeer(kBroadcastMac);
  ESP_LOGI(kTag, "ESP-NOW ready on channel %u", kEspNowChannel);
  return true;
}

void setRecvHandler(RecvHandler handler) { g_handler = handler; }

bool addPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) {
    return true;
  }
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // current WiFi channel
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

bool removePeer(const uint8_t mac[6]) {
  if (!esp_now_is_peer_exist(mac)) {
    return true;
  }
  return esp_now_del_peer(mac) == ESP_OK;
}

bool send(const uint8_t mac[6], const uint8_t *data, size_t len) {
  if (len > kMaxPacketBytes) {
    return false;
  }
  return esp_now_send(mac, data, len) == ESP_OK;
}

bool sendBroadcast(const uint8_t *data, size_t len) { return send(kBroadcastMac, data, len); }

void poll() {
  if (g_rxQueue == nullptr || g_handler == nullptr) {
    return;
  }
  RxItem item;
  while (xQueueReceive(g_rxQueue, &item, 0) == pdTRUE) {
    g_handler(item.mac, item.data, item.len);
  }
}

}  // namespace espnow
