#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/spi_master.h"

// Node with this id is always the chain's Gateway (has the ENC28J60/RJ45
// uplink) -- still a parking-lot node like any other, just the one with this
// extra role. All other ids 1..(chain_size-1) are plain relay nodes (also
// parking lots). Every node runs this same firmware image; role is decided
// at runtime from the provisioned id.
constexpr uint8_t kGatewayNodeId = 0;

// Sanity cap on chain length (provisioning rejects chain_size above this).
// Each StatusPacket carries exactly one node's reading, so unlike the old
// single-collective-token design this is no longer bounded by ESP-NOW's
// 250-byte frame limit -- it's just a generous ceiling for the parking lots
// this is meant to cover. Raise it freely if a real deployment needs more.
constexpr uint8_t kMaxChainNodes = 64;

// Max characters in a StatusPacket's OCR'd plate string (excluding the null
// terminator), e.g. "B1234ABC". Keep spaces stripped at the OCR step on the
// LuckFox side to make more plates fit; this is text only -- plate crop
// images/video never travel over ESP-NOW, see docs/network/topology.md.
constexpr uint8_t kMaxPlateLen = 11;

// Fixed WiFi channel every chain node uses for ESP-NOW. Since there's no AP
// to negotiate a channel, every node must agree on one out of band.
constexpr uint8_t kEspNowChannel = 1;

// -- Discovery (auto-pairing) timing --
constexpr uint32_t kHelloIntervalMs = 500;      // broadcast period while discovering
constexpr uint32_t kHelloIdleIntervalMs = 5000; // slower keep-alive once RUNNING

// -- Chain flow-control timing (main/chain_node.cpp) --
constexpr uint32_t kAckTimeoutMs = 300;   // wait for downstream ACK before retransmit
constexpr uint8_t kMaxRetransmit = 5;     // retries per hop before logging a link warning
constexpr size_t kStatusQueueDepth = 8;   // StatusPackets buffered (local + relayed), in flight to next
constexpr uint32_t kStatusLocalIntervalMs = 2000;  // placeholder producer cadence, see chain_node.cpp

// -- NVS provisioning --
constexpr const char *kNvsNamespace = "rap";
constexpr const char *kNvsKeyNodeId = "node_id";
constexpr const char *kNvsKeyChainSize = "chain_size";
constexpr uint8_t kUnprovisioned = 0xFF;

// -- ENC28J60 SPI wiring (Gateway node only) --
constexpr spi_host_device_t kEncSpiHost = SPI2_HOST;
constexpr int kEncSpiClockMhz = 8;
constexpr int kEncCsPin = 10;
constexpr int kEncSckPin = 12;
constexpr int kEncMisoPin = 13;
constexpr int kEncMosiPin = 11;
constexpr int kEncIntPin = 9;

// Locally-administered MAC for the ENC28J60 side; only needs to be unique on
// the LAN segment the gateway's RJ45 plugs into (ENC28J60 has no burned-in
// MAC of its own).
constexpr uint8_t kEncMac[6] = {0x02, 0x52, 0x41, 0x50, 0x00, 0x01};

// -- Gateway <-> Raspberry Pi 4 Ethernet link (ENC28J60 RJ45 -> RPi4 eth0) --
//
// Direct cable, no switch/router/DHCP server in between, so both ends use
// static IPs instead of DHCP. RPi4's eth0 is otherwise unused (its own LAN/
// internet link is over wlan0) -- see docs/network/topology.md for the
// `nmcli` static-IP setup on the RPi4 side.
constexpr const char *kEthStaticIp = "192.168.50.2";       // this Gateway node
constexpr const char *kEthStaticGateway = "192.168.50.1";  // RPi4 eth0
constexpr const char *kEthStaticNetmask = "255.255.255.0";

// Where each node's occupied+plate reading is sent over Ethernet, one line
// per reading as it arrives (see gateway_uplink::flush()) -- not batched by
// lap/cycle like the old collective-token design was.
// TODO: swap for the real ParkingVision backend's port once that's defined;
// for now this just needs something listening on RPi4:kBackendPort (see
// backend/status_listener.py, outside this submodule, for a minimal demo).
constexpr const char *kBackendHost = kEthStaticGateway;
constexpr uint16_t kBackendPort = 5000;

// Video frames (tagged with origin node/stream, see video_relay.h) go out
// over the same Ethernet link but on their own port/connection, kept open
// rather than reconnected per frame like kBackendPort above -- framing and
// arrival rate are both completely different from the status line.
constexpr uint16_t kVideoBackendPort = 5300;
constexpr size_t kVideoUplinkQueueDepth = 12;  // frames buffered for Ethernet delivery

// -- Video relay (separate radio path from ESP-NOW; see docs/network/topology.md) --
//
// Bulk video from each node's paired LuckFox is far too large for ESP-NOW's
// 250B frames, so it rides regular WiFi/TCP instead: every node runs its own
// SoftAP (so its "prev" neighbor can find & connect to it) and, unless it's
// the Gateway, a STA link into its "next" neighbor's AP -- a WiFi daisy
// chain that mirrors the ESP-NOW chain's node order. Everything stays on
// kEspNowChannel so ESP-NOW keeps working on the same radio.
constexpr uint16_t kVideoPort = 5200;
constexpr const char *kVideoApPassword = "rapvideo1";  // WPA2-PSK needs >=8 chars
constexpr uint8_t kVideoStreamsPerNode = 3;             // 3x 600x400 streams per LuckFox
constexpr size_t kVideoMaxFrameBytes = 32 * 1024;       // sanity cap per frame
constexpr size_t kVideoQueueDepth = 12;                 // frames buffered in PSRAM, in flight to next
constexpr uint32_t kVideoLocalFrameIntervalMs = 300;    // placeholder producer cadence, see video_relay.cpp
