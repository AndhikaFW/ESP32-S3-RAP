#include "chain_node.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "config.h"
#include "espnow_transport.h"
#include "gateway_uplink.h"
#include "protocol.h"

namespace chain_node {

namespace {

constexpr const char *kTag = "chain_node";

enum class State {
  kDiscovering,  // learning neighbor MACs over broadcast HELLO
  kIdle,         // send slot free, ready to pull the next queued packet
  kForwarding,   // a packet is sent to next, waiting for its ACK
};

uint8_t g_nodeId = 0;
uint8_t g_chainSize = 0;
bool g_isGateway = false;

uint8_t g_nextId = 0;
uint8_t g_prevId = 0;
uint8_t g_nextMac[6] = {0};
uint8_t g_prevMac[6] = {0};
bool g_nextKnown = false;
bool g_prevKnown = false;

State g_state = State::kDiscovering;
uint32_t g_lastHelloMs = 0;
uint32_t g_lastLocalMs = 0;
uint16_t g_localSeq = 0;
uint8_t g_localStream = 0;  // rotates 0..kVideoStreamsPerNode-1, one lane per tick

// Dedup key for the last StatusPacket accepted from prev, so a retransmit
// of the same content (because our ACK to prev got lost) just gets re-ACKed
// instead of being queued/forwarded twice.
bool g_hasAcceptedAny = false;
uint8_t g_lastAcceptedOrigin = 0;
uint16_t g_lastAcceptedSeq = 0;

// Packets waiting to go out to `next`: both this node's own local readings
// and whatever it relays from `prev` land in the same FIFO, which is what
// gives the interleaving between "my reading" and "relayed readings" -- no
// separate scheduler needed. Relay nodes only; the Gateway never forwards.
QueueHandle_t g_outQueue = nullptr;

// What's currently sent to `next`, awaiting its ACK.
StatusPacket g_pending{};
uint32_t g_ackDeadlineMs = 0;
uint8_t g_retryCount = 0;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool macEquals(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

// Latest reading per lane, pushed by setLaneStatus() (wired up as
// luckfox_spi's StatusCallback -- see video_relay::init()) as LuckFox's own
// motion -> parked-car check -> occupancy pipeline produces them. Starts at
// "vacant, no plate" per lane, same as an empty spot, until the first real
// reading arrives.
uint8_t g_laneOccupied[kVideoStreamsPerNode] = {0};
char g_lanePlate[kVideoStreamsPerNode][kMaxPlateLen + 1] = {{0}};

// Pulls the latest cached reading for one lane (see g_laneOccupied/
// g_lanePlate above). Every node covers kVideoStreamsPerNode lanes, so the
// local-origination timer in loop() rotates through all of them round-robin
// instead of a single reading per node.
void readLocalStatus(uint8_t streamId, uint8_t *occupied, char plate[kMaxPlateLen + 1]) {
  *occupied = g_laneOccupied[streamId];
  strncpy(plate, g_lanePlate[streamId], kMaxPlateLen);
  plate[kMaxPlateLen] = '\0';
}

void sendAck(const uint8_t *toMac, uint8_t originId, uint16_t seq) {
  AckPacket ack{};
  ack.origin_node_id = originId;
  ack.seq = seq;
  espnow::send(toMac, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
}

void sendStatus(const StatusPacket &pkt) {
  espnow::send(g_nextMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
}

// Puts `pkt` into the outgoing "waiting ACK" slot and transmits it.
void beginForwarding(const StatusPacket &pkt) {
  g_pending = pkt;
  g_retryCount = 0;
  g_ackDeadlineMs = nowMs() + kAckTimeoutMs;
  g_state = State::kForwarding;
  sendStatus(g_pending);
}

void handleHello(const uint8_t *mac, const HelloPacket &hello) {
  if (hello.chain_size != g_chainSize || hello.node_id >= g_chainSize) {
    return;  // mismatched provisioning elsewhere on the network; ignore
  }
  if (hello.node_id == g_nextId && !g_nextKnown) {
    memcpy(g_nextMac, mac, 6);
    espnow::addPeer(g_nextMac);
    g_nextKnown = true;
  }
  if (hello.node_id == g_prevId && !g_prevKnown) {
    memcpy(g_prevMac, mac, 6);
    espnow::addPeer(g_prevMac);
    g_prevKnown = true;
  }

  // The Gateway has no `next` (it flushes to Ethernet instead of forwarding),
  // so it only needs `prev` to be discovered.
  if (g_state == State::kDiscovering && g_prevKnown && (g_isGateway || g_nextKnown)) {
    g_state = State::kIdle;
    if (g_isGateway) {
      ESP_LOGI(kTag, "discovery complete: node %u/%u (gateway, sink only)", g_nodeId, g_chainSize);
    } else {
      ESP_LOGI(kTag, "discovery complete: node %u/%u, next=%02x:%02x:%02x:%02x:%02x:%02x", g_nodeId,
                g_chainSize, g_nextMac[0], g_nextMac[1], g_nextMac[2], g_nextMac[3], g_nextMac[4],
                g_nextMac[5]);
    }
  }
}

void handleStatus(const uint8_t *mac, const StatusPacket &pkt) {
  if (!g_prevKnown || !macEquals(mac, g_prevMac)) {
    return;  // only accept chain traffic from our actual upstream neighbor
  }

  bool isDuplicate = g_hasAcceptedAny && pkt.origin_node_id == g_lastAcceptedOrigin &&
                      pkt.seq == g_lastAcceptedSeq;
  if (isDuplicate) {
    // Our ACK to prev was lost and it retransmitted; just re-ACK -- we
    // already processed (queued/flushed) this content once.
    sendAck(g_prevMac, pkt.origin_node_id, pkt.seq);
    return;
  }

  if (g_isGateway) {
    // Terminus: nothing to queue, always has "room" -- flush straight to
    // Ethernet and ack immediately.
    g_hasAcceptedAny = true;
    g_lastAcceptedOrigin = pkt.origin_node_id;
    g_lastAcceptedSeq = pkt.seq;
    sendAck(g_prevMac, pkt.origin_node_id, pkt.seq);
    gateway_uplink::flush(pkt.origin_node_id, pkt.stream_id, pkt.occupied, pkt.plate);
    return;
  }

  // Relay: only accept (and thus ack) if there's room to queue it for
  // forwarding. If the queue's full we just don't ack -- prev retries after
  // its timeout, which is the backpressure signal that this hop is behind.
  if (xQueueSend(g_outQueue, &pkt, 0) != pdTRUE) {
    return;
  }
  g_hasAcceptedAny = true;
  g_lastAcceptedOrigin = pkt.origin_node_id;
  g_lastAcceptedSeq = pkt.seq;
  sendAck(g_prevMac, pkt.origin_node_id, pkt.seq);
}

void handleAck(const uint8_t *mac, const AckPacket &ack) {
  if (g_state != State::kForwarding || !g_nextKnown || !macEquals(mac, g_nextMac)) {
    return;
  }
  if (ack.origin_node_id != g_pending.origin_node_id || ack.seq != g_pending.seq) {
    return;  // stale ack for a previous send; ignore
  }
  g_state = State::kIdle;
  g_retryCount = 0;
}

void onEspNowRecv(const uint8_t mac[6], const uint8_t *data, size_t len) {
  if (len < 1) return;
  switch (data[0]) {
    case kPktHello:
      if (len >= sizeof(HelloPacket)) {
        HelloPacket hello;
        memcpy(&hello, data, sizeof(hello));
        handleHello(mac, hello);
      }
      break;
    case kPktStatus:
      if (len >= sizeof(StatusPacket)) {
        StatusPacket pkt;
        memcpy(&pkt, data, sizeof(pkt));
        handleStatus(mac, pkt);
      }
      break;
    case kPktAck:
      if (len >= sizeof(AckPacket)) {
        AckPacket ack;
        memcpy(&ack, data, sizeof(ack));
        handleAck(mac, ack);
      }
      break;
    default:
      break;
  }
}

void broadcastHello() {
  HelloPacket hello{};
  hello.node_id = g_nodeId;
  hello.chain_size = g_chainSize;
  espnow::sendBroadcast(reinterpret_cast<const uint8_t *>(&hello), sizeof(hello));
  g_lastHelloMs = nowMs();
}

}  // namespace

void init(uint8_t nodeId, uint8_t chainSize) {
  g_nodeId = nodeId;
  g_chainSize = chainSize;
  g_isGateway = (nodeId == kGatewayNodeId);
  g_nextId = static_cast<uint8_t>((nodeId + 1) % chainSize);
  g_prevId = static_cast<uint8_t>((nodeId + chainSize - 1) % chainSize);
  g_state = State::kDiscovering;

  if (!g_isGateway) {
    g_outQueue = xQueueCreate(kStatusQueueDepth, sizeof(StatusPacket));
  }

  espnow::setRecvHandler(onEspNowRecv);
  broadcastHello();
}

void loop() {
  espnow::poll();

  uint32_t helloInterval = (g_state == State::kDiscovering) ? kHelloIntervalMs : kHelloIdleIntervalMs;
  if (nowMs() - g_lastHelloMs >= helloInterval) {
    broadcastHello();
  }

  if (g_state == State::kDiscovering) {
    return;  // nothing to send/relay until neighbors are known
  }

  // Every node originates its own reading on its own schedule -- no waiting
  // for anything to arrive first.
  if (nowMs() - g_lastLocalMs >= kStatusLocalIntervalMs) {
    g_lastLocalMs = nowMs();
    StatusPacket pkt{};
    pkt.origin_node_id = g_nodeId;
    pkt.stream_id = g_localStream;
    pkt.seq = g_localSeq++;
    readLocalStatus(g_localStream, &pkt.occupied, pkt.plate);
    g_localStream = static_cast<uint8_t>((g_localStream + 1) % kVideoStreamsPerNode);
    if (g_isGateway) {
      gateway_uplink::flush(pkt.origin_node_id, pkt.stream_id, pkt.occupied, pkt.plate);
    } else if (xQueueSend(g_outQueue, &pkt, 0) != pdTRUE) {
      ESP_LOGW(kTag, "out queue full, dropping local reading");
    }
  }

  if (g_isGateway) {
    return;  // pure sink, nothing to forward/retry
  }

  if (g_state == State::kForwarding) {
    if (nowMs() >= g_ackDeadlineMs) {
      g_retryCount++;
      if (g_retryCount > kMaxRetransmit) {
        ESP_LOGW(kTag, "no ACK from next after %u retries (origin=%u seq=%u)", g_retryCount,
                  g_pending.origin_node_id, g_pending.seq);
        // Keep retrying at the same cadence -- there is no self-healing
        // reroute in this version, see docs/network/topology.md TODOs.
      }
      sendStatus(g_pending);
      g_ackDeadlineMs = nowMs() + kAckTimeoutMs;
    }
    return;  // still waiting on this one, don't pull a new item yet
  }

  StatusPacket next;
  if (xQueueReceive(g_outQueue, &next, 0) == pdTRUE) {
    beginForwarding(next);
  }
}

void setLaneStatus(uint8_t stream_id, uint8_t occupied, const char *plate) {
  if (stream_id >= kVideoStreamsPerNode) {
    return;  // out-of-range lane id, e.g. a plate-crop stream_id -- not a status reading
  }
  g_laneOccupied[stream_id] = occupied;
  strncpy(g_lanePlate[stream_id], plate, kMaxPlateLen);
  g_lanePlate[stream_id][kMaxPlateLen] = '\0';
}

}  // namespace chain_node
