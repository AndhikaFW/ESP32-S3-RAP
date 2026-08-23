#include "provisioning.h"

#include <cstdio>
#include <cstring>

#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

namespace provisioning {

namespace {

void printBanner(uint8_t storedId, uint8_t storedChainSize) {
  printf("\n=== ESP32-S3-RAP: node not provisioned ===\n");
  printf("Current NVS values: node_id=%u chain_size=%u\n", storedId, storedChainSize);
  printf("Send:  SETID <node_id> <chain_size>\n");
  printf("  node_id 0   = this node is the Gateway (ENC28J60/RJ45)\n");
  printf("  node_id 1..chain_size-1 = relay node\n");
  printf("Example, node 2 of a 5-node chain:  SETID 2 5\n");
  printf("Device restarts automatically once a valid line is received.\n");
}

bool parseSetId(const char *line, uint8_t *outId, uint8_t *outChainSize) {
  int id = -1;
  int chainSize = -1;
  if (sscanf(line, "SETID %d %d", &id, &chainSize) != 2) {
    return false;
  }
  if (id < 0 || chainSize <= 0 || id >= chainSize || chainSize > kMaxChainNodes) {
    return false;
  }
  *outId = static_cast<uint8_t>(id);
  *outChainSize = static_cast<uint8_t>(chainSize);
  return true;
}

}  // namespace

Identity loadOrProvision() {
  nvs_handle_t handle;
  ESP_ERROR_CHECK(nvs_open(kNvsNamespace, NVS_READWRITE, &handle));

  uint8_t id = kUnprovisioned;
  uint8_t chainSize = kUnprovisioned;
  // ESP_ERR_NVS_NOT_FOUND just leaves id/chainSize at kUnprovisioned.
  nvs_get_u8(handle, kNvsKeyNodeId, &id);
  nvs_get_u8(handle, kNvsKeyChainSize, &chainSize);

  bool valid = (id != kUnprovisioned) && (chainSize != kUnprovisioned) && (id < chainSize) &&
               (chainSize <= kMaxChainNodes);

  if (valid) {
    nvs_close(handle);
    return Identity{id, chainSize};
  }

  printBanner(id, chainSize);
  char line[64];
  while (true) {
    if (fgets(line, sizeof(line), stdin) != nullptr) {
      uint8_t newId = 0;
      uint8_t newChainSize = 0;
      if (parseSetId(line, &newId, &newChainSize)) {
        ESP_ERROR_CHECK(nvs_set_u8(handle, kNvsKeyNodeId, newId));
        ESP_ERROR_CHECK(nvs_set_u8(handle, kNvsKeyChainSize, newChainSize));
        ESP_ERROR_CHECK(nvs_commit(handle));
        nvs_close(handle);
        printf("Saved node_id=%u chain_size=%u, restarting...\n", newId, newChainSize);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
      } else if (strlen(line) > 1) {
        printf("Not understood, expected: SETID <node_id> <chain_size>\n");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace provisioning
