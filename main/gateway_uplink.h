#pragma once

#include <cstdint>

#include "protocol.h"

// ENC28J60 (SPI Ethernet -> RJ45) side of the Gateway node. Only ever used
// on the node whose provisioned id == kGatewayNodeId; relay nodes never
// touch this module.
namespace gateway_uplink {

// Brings up the SPI bus, ENC28J60 MAC/PHY, esp_netif and DHCP client.
// Assumes esp_netif_init()/esp_event_loop_create_default() already ran.
// Returns false if the Ethernet driver could not be installed/started; the
// ring still runs, entries just fail to flush until the link recovers.
bool init();

// Sends one completed lap's worth of entries to the backend server over a
// plain TCP socket. Safe to call even if the link/DHCP isn't up yet (it
// will just fail fast and log).
void flush(const TokenEntry *entries, uint8_t count, uint16_t cycleId);

}  // namespace gateway_uplink
