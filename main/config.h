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
//
// SPI3_HOST here (not SPI2_HOST) -- see the LuckFox SPI link section below
// for why: ESP32-S3 only has one SPI controller with a dedicated IOMUX fast
// path (SPI2_HOST, pins GPIO10/11/12/13), and the LuckFox link needs it far
// more (it's the SPI *slave*, which is unreliable on the GPIO-matrix-routed
// host). ENC28J60 is master-only here and tolerates the GPIO matrix's extra
// routing delay fine, so it gives up the IOMUX pins in this swap. On the
// real shield PCB this is moot (both are fixed traces); on a dev-board
// breadboard, rewire ENC28J60's 4 SPI wires to GPIO4/5/6/7 (CS/SCK/MOSI/MISO
// respectively) to match.
constexpr spi_host_device_t kEncSpiHost = SPI3_HOST;
// Tried lowering this to 2MHz to chase intermittent tx_ready_sem timeouts
// (packets never finishing transmission) on the breadboard rig -- made no
// difference (identical failure at 2MHz and 8MHz), so the timeout isn't an
// SPI signal-integrity issue; back to 8MHz since this chip's silicon
// revision (B7) is rated for it. See docs/network/enc28j60_wiring.dot --
// the actual cause looks like the breadboard ENC28J60 module itself
// (no proper magnetics/RJ45 circuit).
constexpr int kEncSpiClockMhz = 8;
constexpr int kEncCsPin = 7;
constexpr int kEncSckPin = 6;
constexpr int kEncMisoPin = 5;
constexpr int kEncMosiPin = 4;
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
// 512KB: sized for the PoC's PNG frames (no JPEG encoder available on the
// LuckFox test image yet, see main/luckfox_spi.py -- a 600x400 PNG frame
// measured ~366KB in practice) -- shrink back down once real JPEG/H.264
// encoding lands on the LuckFox side (original placeholder assumed ~15-23KB
// JPEG frames).
constexpr size_t kVideoMaxFrameBytes = 512 * 1024;
constexpr size_t kVideoQueueDepth = 12;                 // frames buffered in PSRAM, in flight to next
constexpr uint32_t kVideoLocalFrameIntervalMs = 300;    // placeholder producer cadence, see video_relay.cpp

// -- LuckFox <-> ESP32 SPI link (every node, not just the Gateway) --
//
// ESP32-S3 is the SPI *slave* here (driver/spi_slave.h) -- LuckFox's Linux
// side already owns /dev/spidev0.0 in master mode (enabled via
// `luckfox-config`'s SPI0 M0 overlay; Linux spidev is master-only, and this
// firmware already uses spi_master.h for the Gateway's ENC28J60, so LuckFox
// stays master and ESP32 is the slave here to avoid a two-master bus).
//
// SPI2_HOST specifically (not SPI3_HOST) -- ESP32-S3 only gives ONE SPI
// controller a dedicated IOMUX fast path (SPI2_HOST: CS0=GPIO10, SCK=GPIO12,
// MOSI/FSPID=GPIO11, MISO/FSPIQ=GPIO13); SPI3_HOST has no IOMUX pins at all
// and is always routed through the slower/less precise GPIO matrix. That's
// fine for a *master* (it drives its own clock and tolerates the extra
// delay), but ESP-IDF's SPI *slave* driver needs to sample an externally-
// generated clock and is known to be unreliable -- transactions can fail to
// complete at all, regardless of clock speed -- when routed through the GPIO
// matrix instead of IOMUX. Root-caused after the ENC28J60 (a master) sat on
// these same IOMUX pins and worked fine there while the LuckFox slave link
// on arbitrary GPIO4/5/6/7 (GPIO-matrix-routed) never completed a single
// transaction despite every other layer (wiring, LuckFox-side spidev
// loopback, this firmware's own GPIO-level loopback) checking out perfectly.
// See ENC28J60 section above for its swapped-out pins.
constexpr spi_host_device_t kLuckfoxSpiHost = SPI2_HOST;
constexpr int kLuckfoxSpiMosiPin = 11;
constexpr int kLuckfoxSpiMisoPin = 13;
constexpr int kLuckfoxSpiSckPin = 12;
constexpr int kLuckfoxSpiCsPin = 10;
// Per-transaction chunk size for the slave's DMA-backed recv buffer. Frames
// larger than this are split into ceil(len/chunk) chunks by the LuckFox-side
// sender (see main/luckfox_spi.py); must match on both ends.
constexpr size_t kLuckfoxSpiChunkBytes = 4000;
