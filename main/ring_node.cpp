#include "ring_node.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "espnow_transport.h"
#include "gateway_uplink.h"
#include "protocol.h"

namespace ring_node {

namespace {

constexpr const char *kTag = "ring_node";

enum class State {
  kDiscovering,  // learning neighbor MACs over broadcast HELLO
  kIdle,         // send slot free, waiting to receive the token from prev
  kForwarding,   // token sent to next, waiting for its ACK
};

uint8_t g_nodeId = 0;
uint8_t g_ringSize = 0;
bool g_isGateway = false;

uint8_t g_nextId = 0;
uint8_t g_prevId = 0;
uint8_t g_nextMac[6] = {0};
uint8_t g_prevMac[6] = {0};
bool g_nextKnown = false;
bool g_prevKnown = false;

State g_state = State::kDiscovering;
uint32_t g_lastHelloMs = 0;

// Dedup key for the last token this node accepted from prev, so a
// retransmit of the same content (because our ACK to prev got lost) just
// gets re-ACKed instead of being processed/forwarded twice.
uint16_t g_lastAcceptedCycle = 0xFFFF;
uint8_t g_lastAcceptedCount = 0xFF;
bool g_hasAcceptedAny = false;

// What we're currently holding in the "sent to next, awaiting ACK" slot.
TokenPacket g_pending{};
uint32_t g_ackDeadlineMs = 0;
uint8_t g_retryCount = 0;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool macEquals(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

// Placeholder for the real ParkingVision sensor reading. Swap this out for
// actual sensor/occupancy logic; kept as a visible counter so a lap's
// contents change from run to run during bring-up.
uint8_t readLocalValue() {
  static uint8_t counter = 0;
  return counter++;
}

void sendAck(const uint8_t *toMac, uint16_t cycleId, uint8_t count) {
  AckPacket ack{};
  ack.cycle_id = cycleId;
  ack.count = count;
  espnow::send(toMac, reinterpret_cast<const uint8_t *>(&ack), sizeof(ack));
}

size_t tokenWireSize(const TokenPacket &pkt) {
  // Only the filled prefix of entries[] actually needs to go over the air.
  return sizeof(TokenPacket) - (kMaxRingNodes - pkt.count) * sizeof(TokenEntry);
}

void sendToken(const TokenPacket &pkt) {
  espnow::send(g_nextMac, reinterpret_cast<const uint8_t *>(&pkt), tokenWireSize(pkt));
}

// Applies this node's role-specific transform to a just-accepted token and
// puts the result into the outgoing "waiting ACK" slot, then transmits it.
// bootstrap is true only for the synthetic lap-0 receipt a Gateway invents
// at startup to kick off cycle_id = 1 -- nothing real to flush there.
void beginForwarding(const TokenPacket &accepted, bool bootstrap = false) {
  TokenPacket outgoing = accepted;

  if (g_isGateway) {
    if (!bootstrap) {
      gateway_uplink::flush(accepted.entries, accepted.count, accepted.cycle_id);
    }
    outgoing.cycle_id = accepted.cycle_id + 1;
    outgoing.count = 0;
  } else if (outgoing.count < kMaxRingNodes) {
    outgoing.entries[outgoing.count].node_id = g_nodeId;
    outgoing.entries[outgoing.count].value = readLocalValue();
    outgoing.count++;
  }

  g_pending = outgoing;
  g_retryCount = 0;
  g_ackDeadlineMs = nowMs() + kAckTimeoutMs;
  g_state = State::kForwarding;
  sendToken(g_pending);
}

void handleHello(const uint8_t *mac, const HelloPacket &hello) {
  if (hello.ring_size != g_ringSize || hello.node_id >= g_ringSize) {
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

  if (g_state == State::kDiscovering && g_nextKnown && g_prevKnown) {
    g_state = State::kIdle;
    ESP_LOGI(kTag, "discovery complete: node %u/%u, next=%02x:%02x:%02x:%02x:%02x:%02x", g_nodeId,
             g_ringSize, g_nextMac[0], g_nextMac[1], g_nextMac[2], g_nextMac[3], g_nextMac[4],
             g_nextMac[5]);
    if (g_isGateway) {
      // Nothing has "arrived" yet -- synthesize an empty lap-0 receipt so
      // the very first real lap (cycle_id = 1) gets kicked off.
      TokenPacket bootstrapToken{};
      bootstrapToken.cycle_id = 0;
      bootstrapToken.count = 0;
      g_hasAcceptedAny = true;
      g_lastAcceptedCycle = 0;
      g_lastAcceptedCount = 0;
      beginForwarding(bootstrapToken, /*bootstrap=*/true);
    }
  }
}

void handleToken(const uint8_t *mac, const TokenPacket &token) {
  if (!g_prevKnown || !macEquals(mac, g_prevMac)) {
    return;  // only accept ring traffic from our actual upstream neighbor
  }

  bool isDuplicate =
      g_hasAcceptedAny && token.cycle_id == g_lastAcceptedCycle && token.count == g_lastAcceptedCount;

  if (isDuplicate) {
    // Our ACK to prev was lost and it retransmitted; just re-ACK, whatever
    // state we're in now -- we already processed this content once.
    sendAck(g_prevMac, token.cycle_id, token.count);
    return;
  }

  if (g_state != State::kIdle) {
    // New content arriving while our own send slot is still occupied means
    // prev got ahead of the protocol (or a stale duplicate slipped past the
    // dedup check above); drop it, prev will retransmit once we're free.
    return;
  }

  g_hasAcceptedAny = true;
  g_lastAcceptedCycle = token.cycle_id;
  g_lastAcceptedCount = token.count;
  sendAck(g_prevMac, token.cycle_id, token.count);
  beginForwarding(token);
}

void handleAck(const uint8_t *mac, const AckPacket &ack) {
  if (g_state != State::kForwarding || !g_nextKnown || !macEquals(mac, g_nextMac)) {
    return;
  }
  if (ack.cycle_id != g_pending.cycle_id || ack.count != g_pending.count) {
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
    case kPktToken: {
      // Variable-length: only [0, count) entries were actually sent.
      size_t headerSize = sizeof(TokenPacket) - kMaxRingNodes * sizeof(TokenEntry);
      if (len < headerSize) break;
      TokenPacket token{};
      memcpy(&token, data, headerSize);
      size_t entryBytes = len - headerSize;
      size_t maxEntryBytes = kMaxRingNodes * sizeof(TokenEntry);
      if (entryBytes > maxEntryBytes) entryBytes = maxEntryBytes;
      memcpy(token.entries, data + headerSize, entryBytes);
      handleToken(mac, token);
      break;
    }
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
  hello.ring_size = g_ringSize;
  espnow::sendBroadcast(reinterpret_cast<const uint8_t *>(&hello), sizeof(hello));
  g_lastHelloMs = nowMs();
}

}  // namespace

void init(uint8_t nodeId, uint8_t ringSize) {
  g_nodeId = nodeId;
  g_ringSize = ringSize;
  g_isGateway = (nodeId == kGatewayNodeId);
  g_nextId = static_cast<uint8_t>((nodeId + 1) % ringSize);
  g_prevId = static_cast<uint8_t>((nodeId + ringSize - 1) % ringSize);
  g_state = State::kDiscovering;

  espnow::setRecvHandler(onEspNowRecv);
  broadcastHello();
}

void loop() {
  espnow::poll();

  uint32_t interval = (g_state == State::kDiscovering) ? kHelloIntervalMs : kHelloIdleIntervalMs;
  if (nowMs() - g_lastHelloMs >= interval) {
    broadcastHello();
  }

  if (g_state == State::kForwarding && nowMs() >= g_ackDeadlineMs) {
    g_retryCount++;
    if (g_retryCount > kMaxRetransmit) {
      ESP_LOGW(kTag, "no ACK from next after %u retries (cycle=%u count=%u)", g_retryCount,
                g_pending.cycle_id, g_pending.count);
      // Keep retrying at the same cadence -- there is no self-healing
      // reroute in this version, see docs/network/topology.md TODOs.
    }
    sendToken(g_pending);
    g_ackDeadlineMs = nowMs() + kAckTimeoutMs;
  }
}

}  // namespace ring_node
