#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "espnow_transport.h"
#include "gateway_uplink.h"
#include "provisioning.h"
#include "ring_node.h"

namespace {
constexpr const char *kTag = "main";
}

extern "C" void app_main(void) {
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvsErr = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvsErr);

  // Shared by both ESP-NOW (WiFi) and the Gateway's Ethernet netif, so this
  // runs once here rather than inside espnow_transport/gateway_uplink.
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  provisioning::Identity identity = provisioning::loadOrProvision();
  ESP_LOGI(kTag, "node_id=%u ring_size=%u role=%s", identity.node_id, identity.ring_size,
           identity.node_id == kGatewayNodeId ? "GATEWAY" : "RELAY");

  if (!espnow::init()) {
    ESP_LOGE(kTag, "ESP-NOW init failed, halting");
    while (true) {
      vTaskDelay(portMAX_DELAY);
    }
  }

  if (identity.node_id == kGatewayNodeId) {
    if (!gateway_uplink::init()) {
      ESP_LOGW(kTag, "gateway Ethernet init failed, continuing without uplink");
    }
  }

  ring_node::init(identity.node_id, identity.ring_size);

  while (true) {
    ring_node::loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
