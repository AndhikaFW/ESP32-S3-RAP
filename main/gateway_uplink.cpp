#include "gateway_uplink.h"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_enc28j60.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "config.h"

namespace gateway_uplink {

namespace {

constexpr const char *kTag = "gateway_uplink";

esp_eth_handle_t g_ethHandle = nullptr;
esp_netif_t *g_netif = nullptr;
volatile bool g_linkUp = false;

void ethEventHandler(void *, esp_event_base_t, int32_t eventId, void *eventData) {
  auto handle = *static_cast<esp_eth_handle_t *>(eventData);
  uint8_t mac[6] = {0};
  switch (eventId) {
    case ETHERNET_EVENT_CONNECTED:
      g_linkUp = true;
      esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
      ESP_LOGI(kTag, "link up, MAC=%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5]);
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      g_linkUp = false;
      ESP_LOGW(kTag, "link down");
      break;
    default:
      break;
  }
}

void gotIpEventHandler(void *, esp_event_base_t, int32_t, void *eventData) {
  auto *event = static_cast<ip_event_got_ip_t *>(eventData);
  ESP_LOGI(kTag, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
}

}  // namespace

bool init() {
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
  // ENC28J60 Errata #1: silicon revisions below B5 need >=8MHz SPI clock.
  if (emac_enc28j60_get_chip_info(mac) < ENC28J60_REV_B5 && kEncSpiClockMhz < 8) {
    ESP_LOGE(kTag, "SPI clock must be >=8MHz for this ENC28J60 silicon revision");
    mac->del(mac);
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

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &gotIpEventHandler, nullptr);

  if (esp_eth_start(g_ethHandle) != ESP_OK) {
    ESP_LOGE(kTag, "esp_eth_start failed");
    return false;
  }
  return true;
}

void flush(const TokenEntry *entries, uint8_t count, uint16_t cycleId) {
  if (!g_linkUp) {
    ESP_LOGW(kTag, "link down, dropping lap %u", cycleId);
    return;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGW(kTag, "socket() failed, dropping lap %u", cycleId);
    return;
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(kBackendPort);
  inet_pton(AF_INET, kBackendHost, &dest.sin_addr);

  // 2s connect/send budget so a dead backend never stalls the ring.
  timeval tv{};
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (connect(sock, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) != 0) {
    ESP_LOGW(kTag, "connect to %s:%u failed, dropping lap %u", kBackendHost, kBackendPort, cycleId);
    close(sock);
    return;
  }

  // Compact line-oriented payload: "cycle,count;node:value,node:value,...\n"
  // Swap this for whatever framing the ParkingVision backend expects once
  // that API is defined.
  char line[16 + kMaxRingNodes * 8];
  int written = snprintf(line, sizeof(line), "%u,%u;", cycleId, count);
  for (uint8_t i = 0; i < count && written < static_cast<int>(sizeof(line)); ++i) {
    written += snprintf(line + written, sizeof(line) - written, "%u:%u%s", entries[i].node_id,
                         entries[i].value, (i + 1 < count) ? "," : "");
  }
  if (written < static_cast<int>(sizeof(line))) {
    line[written++] = '\n';
  }

  send(sock, line, written, 0);
  close(sock);
}

}  // namespace gateway_uplink
