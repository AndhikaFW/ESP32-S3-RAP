#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "espnow_transport.h"
#include "gateway_uplink.h"
#include "provisioning.h"
#include "chain_node.h"
#include "video_relay.h"

namespace {
constexpr const char *kTag = "main";
}

// Boot sequence: NVS -> shared netif/event loop -> WiFi netifs (video_relay
// needs both created before esp_wifi_start(), which espnow::init() below
// calls) -> load this unit's node_id/chain_size (blocks on console UART if
// not yet provisioned) -> ESP-NOW -> Ethernet (Gateway only) -> chain state
// machine -> video relay -> the main polling loop, which just drives
// chain_node's timers/RX; video_relay and gateway_uplink run their own
// FreeRTOS tasks and don't need to be polled here.
extern "C" void app_main(void) {
  ESP_LOGI(kTag, "app_main enter");
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvsErr = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvsErr);

  // Shared by ESP-NOW, video_relay's AP+STA, and the Gateway's Ethernet
  // netif, so this runs once here rather than inside each module.
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // video_relay needs both netifs (AP for `prev` to join, STA to join
  // `next`) created before esp_wifi_start(); ESP-NOW itself doesn't need
  // either, but shares the same WIFI_MODE_APSTA driver instance.
  ESP_LOGI(kTag, "checkpoint: netif/event_loop ok");
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();
  ESP_LOGI(kTag, "checkpoint: default wifi netifs created");

  provisioning::Identity identity = provisioning::loadOrProvision();
  ESP_LOGI(kTag, "node_id=%u chain_size=%u role=%s", identity.node_id, identity.chain_size,
           identity.node_id == kGatewayNodeId ? "GATEWAY" : "RELAY");

  if (!espnow::init()) {
    ESP_LOGE(kTag, "ESP-NOW init failed, halting");
    while (true) {
      vTaskDelay(portMAX_DELAY);
    }
  }

  if (identity.node_id == kGatewayNodeId) {
    ESP_LOGI(kTag, "checkpoint: before gateway_uplink::init");
    if (!gateway_uplink::init()) {
      ESP_LOGW(kTag, "gateway Ethernet init failed, continuing without uplink");
    }
    ESP_LOGI(kTag, "checkpoint: after gateway_uplink::init");
  }

  chain_node::init(identity.node_id, identity.chain_size);
  ESP_LOGI(kTag, "checkpoint: before video_relay::init");
  video_relay::init(identity.node_id, identity.chain_size, identity.node_id == kGatewayNodeId);
  ESP_LOGI(kTag, "checkpoint: after video_relay::init");

  while (true) {
    chain_node::loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
