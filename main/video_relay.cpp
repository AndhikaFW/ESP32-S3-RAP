#include "video_relay.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

#include "chain_node.h"
#include "config.h"
#include "gateway_uplink.h"
#include "luckfox_spi.h"

namespace video_relay {

namespace {

constexpr const char *kTag = "video_relay";

// Every node's SoftAP gets its own /24 based on its node_id (192.168.(10+id).1)
// instead of ESP-IDF's identical default 192.168.4.1 on every node. With the
// default, "connect to 192.168.4.1" was ambiguous: it matched this node's
// OWN AP interface (which also sits at 192.168.4.1) just as well as the
// peer's, so the client silently looped back to its own server instead of
// reaching `next` -- connect() reported success, but the peer never saw an
// incoming connection. Per-node subnets make the address unambiguous.
constexpr uint8_t kApSubnetBase = 10;

uint8_t g_nodeId = 0;
uint8_t g_chainSize = 0;
uint8_t g_nextId = 0;
bool g_isGateway = false;
char g_nextIp[16] = {0};  // "next" node's AP IP, e.g. "192.168.11.1"

// Health counters for logStats(); written from several tasks, only read for
// the stats line, so torn/stale reads are harmless and no locking is used.
std::atomic<uint32_t> g_vLocal{0};      // frames produced by this node's LuckFox (or the self-test)
std::atomic<uint32_t> g_vIn{0};         // frames received from prev
std::atomic<uint32_t> g_vOut{0};        // frames sent to next
std::atomic<uint32_t> g_vOutBytes{0};
std::atomic<uint32_t> g_vDrop{0};       // dropped: queue full / alloc failed
std::atomic<uint32_t> g_vSunk{0};       // Gateway: frames handed to the uplink
std::atomic<uint32_t> g_vBad{0};        // Gateway self-test: frames whose payload did not verify
std::atomic<uint32_t> g_vReconnects{0}; // client connects to next (after the first = recoveries)
std::atomic<uint32_t> g_vPrevDrops{0};  // prev connections lost (timeout / reset)
volatile bool g_serverUp = false;    // prev currently connected to our server
volatile bool g_clientUp = false;    // we are currently connected to next

// Frames waiting to go out to `next`: both this node's own local frames and
// whatever it received from `prev` land in the same FIFO, which is what
// gives the round-robin interleaving between "my frames" and "relayed
// frames" -- no separate scheduler needed. Relay nodes only.
QueueHandle_t g_outQueue = nullptr;

struct __attribute__((packed)) FrameHeader {
  uint8_t origin_node_id;
  uint8_t stream_id;  // 0..kVideoStreamsPerNode-1
  uint16_t seq;
  uint32_t data_len;
};

struct Frame {
  FrameHeader header;
  uint8_t *data;  // heap_caps_malloc'd from PSRAM
};

void freeFrame(Frame *frame) {
  if (frame == nullptr) return;
  if (frame->data != nullptr) heap_caps_free(frame->data);
  delete frame;
}

// PSRAM first (that's the whole 8MB buffering budget), falling back to
// internal heap if it's unavailable -- e.g. a node whose physical PSRAM
// chip is defective still gets a small in-flight window instead of
// dropping every single frame it touches.
uint8_t *allocFrameBuffer(size_t size) {
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    buf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
  }
  return buf;
}

// This node is the Gateway, or this frame was produced locally by the
// Gateway's own paired LuckFox -- there's nowhere further to relay video
// to, so hand it to gateway_uplink for delivery over Ethernet instead
// (frame->header already carries origin_node_id/stream_id/seq, so the
// backend can always tell which node/camera/frame this is, the same as
// StatusPacket's plate readings via gateway_uplink::flush()).
// Self-test payload: byte i of a frame is a pure function of (origin, stream,
// seq, i), so the Gateway can verify any frame that arrives -- whichever
// hop it crossed -- without needing a copy to compare against.
inline uint8_t selfTestByte(uint8_t origin, uint8_t stream, uint16_t seq, size_t i) {
  return static_cast<uint8_t>(seq + stream * 17 + origin * 3 + i * 7);
}

bool selfTestFrameOk(const Frame *frame) {
  const FrameHeader &h = frame->header;
  if (h.data_len != kSelfTestFrameBytes) return false;
  for (size_t i : {size_t{0}, size_t{1}, size_t{h.data_len / 2}, size_t{h.data_len - 1}}) {
    if (frame->data[i] != selfTestByte(h.origin_node_id, h.stream_id, h.seq, i)) return false;
  }
  return true;
}

void sinkFrame(Frame *frame) {
  g_vSunk++;
  if (kChainSelfTest && !selfTestFrameOk(frame)) {
    g_vBad++;
  }
  gateway_uplink::flushVideo(frame->header.origin_node_id, frame->header.stream_id, frame->header.seq,
                              frame->data, frame->header.data_len);
  frame->data = nullptr;  // ownership just transferred to gateway_uplink
  freeFrame(frame);
}

// Relay nodes: hand a frame (local or relayed) to the outgoing queue toward
// `next`. Drops it if the queue is backed up rather than blocking forever --
// a stalled `next` link shouldn't wedge the frame producer/receiver.
void enqueueOrDrop(Frame *frame) {
  if (g_isGateway) {
    sinkFrame(frame);
    return;
  }
  if (xQueueSend(g_outQueue, &frame, pdMS_TO_TICKS(50)) != pdTRUE) {
    g_vDrop++;  // counted in logStats(); a stalled hop would otherwise log per frame
    freeFrame(frame);
  }
}

bool recvAll(int sock, void *buf, size_t len) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(sock, p + got, len - got, 0);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

// Plain blocking connect() can sit for tens of seconds before giving up on
// an unreachable/not-yet-associated peer, which starved this task of a
// chance to retry (and of logging anything) while `next`'s AP wasn't up
// yet. Bound it explicitly so retries actually happen every couple seconds.
bool connectWithTimeout(int sock, const sockaddr_in &dest, int timeoutMs) {
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  int rc = connect(sock, reinterpret_cast<const sockaddr *>(&dest), sizeof(dest));
  if (rc == 0) {
    fcntl(sock, F_SETFL, flags);
    return true;
  }
  if (errno != EINPROGRESS) {
    fcntl(sock, F_SETFL, flags);
    return false;
  }

  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(sock, &writeSet);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;

  rc = select(sock + 1, nullptr, &writeSet, nullptr, &tv);
  fcntl(sock, F_SETFL, flags);
  if (rc <= 0) {
    return false;  // timed out or select() error
  }

  int sockErr = 0;
  socklen_t len = sizeof(sockErr);
  getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockErr, &len);
  return sockErr == 0;
}

bool sendAll(int sock, const void *buf, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(sock, p + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// -------------------------------------------------------------------------
// Server side: SoftAP + TCP server accepting `prev`'s connection, receiving
// whatever it relays (or -- for the very first hop -- its own local
// frames arriving indirectly makes no sense, prev always frames it).
// -------------------------------------------------------------------------
void serverTask(void * /*arg*/) {
  int listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenSock < 0) {
    ESP_LOGE(kTag, "server socket() failed");
    vTaskDelete(nullptr);
    return;
  }
  int reuse = 1;
  setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kVideoPort);
  if (bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      listen(listenSock, 1) != 0) {
    ESP_LOGE(kTag, "server bind/listen failed");
    close(listenSock);
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(kTag, "video server listening on :%u", kVideoPort);

  while (true) {
    int client = accept(listenSock, nullptr, nullptr);
    if (client < 0) continue;
    ESP_LOGI(kTag, "prev connected");
    g_serverUp = true;

    // A silent peer (reset/powered off: no FIN) must not leave this blocking
    // recv() hanging forever -- see kVideoRecvTimeoutMs in config.h.
    timeval rcvTimeout{};
    rcvTimeout.tv_sec = kVideoRecvTimeoutMs / 1000;
    rcvTimeout.tv_usec = (kVideoRecvTimeoutMs % 1000) * 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));

    while (true) {
      FrameHeader hdr;
      if (!recvAll(client, &hdr, sizeof(hdr))) break;
      if (hdr.stream_id == kVideoHeartbeatStream && hdr.data_len == 0) {
        continue;  // keepalive from prev, nothing to relay
      }
      if (hdr.data_len == 0 || hdr.data_len > kVideoMaxFrameBytes) {
        ESP_LOGW(kTag, "bad frame header (len=%u), dropping connection", hdr.data_len);
        break;
      }

      auto *data = allocFrameBuffer(hdr.data_len);
      if (data == nullptr) {
        g_vDrop++;
        ESP_LOGW(kTag, "frame alloc failed (%u bytes), dropping frame", hdr.data_len);
        // Still have to drain the payload off the socket to stay framed.
        uint8_t scratch[256];
        uint32_t remaining = hdr.data_len;
        while (remaining > 0) {
          size_t chunk = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
          if (!recvAll(client, scratch, chunk)) break;
          remaining -= chunk;
        }
        continue;
      }
      if (!recvAll(client, data, hdr.data_len)) {
        heap_caps_free(data);
        break;
      }

      g_vIn++;
      enqueueOrDrop(new Frame{hdr, data});
    }

    close(client);
    g_serverUp = false;
    g_vPrevDrops++;
    ESP_LOGW(kTag, "prev disconnected");
  }
}

// -------------------------------------------------------------------------
// Client side (relay nodes only): STA joined to `next`'s AP, TCP client
// draining g_outQueue toward it. Reconnects on failure.
// -------------------------------------------------------------------------
void clientTask(void * /*arg*/) {
  while (true) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kVideoPort);
    inet_pton(AF_INET, g_nextIp, &dest.sin_addr);

    if (!connectWithTimeout(sock, dest, 2000)) {
      ESP_LOGW(kTag, "connect() to %s:%u failed/timed out: errno=%d", g_nextIp, kVideoPort, errno);
      close(sock);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    ESP_LOGI(kTag, "connected to next (node %u)", g_nextId);
    g_clientUp = true;
    g_vReconnects++;

    // Don't let a stalled receiver block send() forever either.
    timeval sndTimeout{};
    sndTimeout.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sndTimeout, sizeof(sndTimeout));

    bool linkOk = true;
    while (linkOk) {
      Frame *frame = nullptr;
      if (xQueueReceive(g_outQueue, &frame, pdMS_TO_TICKS(kVideoHeartbeatMs)) != pdTRUE) {
        // Idle: say so, so the receiver's recv timeout can tell "quiet" from "dead".
        FrameHeader heartbeat{0, kVideoHeartbeatStream, 0, 0};
        linkOk = sendAll(sock, &heartbeat, sizeof(heartbeat));
        continue;
      }
      uint32_t len = frame->header.data_len;
      linkOk = sendAll(sock, &frame->header, sizeof(frame->header)) &&
               sendAll(sock, frame->data, len);
      if (linkOk) {
        g_vOut++;
        g_vOutBytes += len;
      } else {
        g_vDrop++;
      }
      freeFrame(frame);
    }

    g_clientUp = false;
    close(sock);
    ESP_LOGW(kTag, "lost connection to next, retrying");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// -------------------------------------------------------------------------
// Local source: frames arriving over SPI from this node's paired LuckFox
// (see main/luckfox_spi.*). Runs on luckfox_spi's own receiver task context,
// not a task of its own here -- just wraps the raw bytes in a FrameHeader
// and drops them on the same queue as relayed frames.
// -------------------------------------------------------------------------
void onLuckfoxFrame(uint8_t stream_id, uint16_t seq, uint8_t *data, uint32_t len) {
  g_vLocal++;
  FrameHeader hdr{g_nodeId, stream_id, seq, len};
  enqueueOrDrop(new Frame{hdr, data});
}

// kChainSelfTest only: stands in for the LuckFox so the chain can be tested
// with nothing but ESP32 boards. One frame per kVideoLocalFrameIntervalMs,
// rotating through the lanes, payload verifiable at the Gateway.
void selfTestProducerTask(void * /*arg*/) {
  uint16_t seq[kVideoStreamsPerNode] = {0};
  uint8_t stream = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(kVideoLocalFrameIntervalMs));
    uint8_t *data = allocFrameBuffer(kSelfTestFrameBytes);
    if (data == nullptr) {
      g_vDrop++;
      continue;
    }
    uint16_t s = seq[stream]++;
    for (size_t i = 0; i < kSelfTestFrameBytes; i++) {
      data[i] = selfTestByte(g_nodeId, stream, s, i);
    }
    onLuckfoxFrame(stream, s, data, kSelfTestFrameBytes);
    stream = static_cast<uint8_t>((stream + 1) % kVideoStreamsPerNode);
  }
}

void wifiEventHandler(void * /*arg*/, esp_event_base_t base, int32_t id, void *data) {
  if (base != WIFI_EVENT) return;
  if (id == WIFI_EVENT_STA_DISCONNECTED) {
    auto *info = static_cast<wifi_event_sta_disconnected_t *>(data);
    ESP_LOGW(kTag, "STA disconnected from next's AP, reason=%d", info->reason);
    if (!g_isGateway) {
      esp_wifi_connect();  // keep retrying the join to next's AP
    }
  } else if (id == WIFI_EVENT_STA_CONNECTED) {
    ESP_LOGI(kTag, "STA associated with next's AP (WiFi layer)");
  } else if (id == WIFI_EVENT_AP_STACONNECTED) {
    ESP_LOGI(kTag, "a station associated with our AP (WiFi layer)");
  }
}

}  // namespace

void init(uint8_t nodeId, uint8_t chainSize, bool isGateway) {
  g_nodeId = nodeId;
  g_chainSize = chainSize;
  g_isGateway = isGateway;
  g_nextId = static_cast<uint8_t>((nodeId + 1) % chainSize);
  snprintf(g_nextIp, sizeof(g_nextIp), "192.168.%u.1", kApSubnetBase + g_nextId);

  // Give this node's own SoftAP a subnet unique to its node_id (see
  // kApSubnetBase comment) instead of ESP-IDF's identical 192.168.4.1
  // default on every node.
  esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apNetif != nullptr) {
    esp_netif_dhcps_stop(apNetif);
    esp_netif_ip_info_t ipInfo{};
    IP4_ADDR(&ipInfo.ip, 192, 168, kApSubnetBase + nodeId, 1);
    IP4_ADDR(&ipInfo.gw, 192, 168, kApSubnetBase + nodeId, 1);
    IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(apNetif, &ipInfo);
    esp_netif_dhcps_start(apNetif);
  } else {
    ESP_LOGE(kTag, "could not find WIFI_AP_DEF netif to set static IP");
  }

  // This node's own identity, so its `prev` neighbor can find it.
  char apSsid[16];
  snprintf(apSsid, sizeof(apSsid), "RAP-%u", nodeId);
  wifi_config_t apConfig{};
  strncpy(reinterpret_cast<char *>(apConfig.ap.ssid), apSsid, sizeof(apConfig.ap.ssid));
  apConfig.ap.ssid_len = static_cast<uint8_t>(strlen(apSsid));
  strncpy(reinterpret_cast<char *>(apConfig.ap.password), kVideoApPassword,
          sizeof(apConfig.ap.password));
  apConfig.ap.channel = kEspNowChannel;
  apConfig.ap.authmode = WIFI_AUTH_WPA2_PSK;
  apConfig.ap.max_connection = 1;
  esp_wifi_set_config(WIFI_IF_AP, &apConfig);

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);

  if (!isGateway) {
    char nextSsid[16];
    snprintf(nextSsid, sizeof(nextSsid), "RAP-%u", g_nextId);
    wifi_config_t staConfig{};
    strncpy(reinterpret_cast<char *>(staConfig.sta.ssid), nextSsid, sizeof(staConfig.sta.ssid));
    strncpy(reinterpret_cast<char *>(staConfig.sta.password), kVideoApPassword,
            sizeof(staConfig.sta.password));
    staConfig.sta.channel = kEspNowChannel;
    esp_wifi_set_config(WIFI_IF_STA, &staConfig);
    esp_wifi_connect();

    g_outQueue = xQueueCreate(kVideoQueueDepth, sizeof(Frame *));
    xTaskCreate(clientTask, "video_client", 4096, nullptr, 5, nullptr);
  }

  xTaskCreate(serverTask, "video_server", 4096, nullptr, 5, nullptr);
  if (kChainSelfTest) {
    ESP_LOGW(kTag, "SELF-TEST MODE: synthetic frames/readings, LuckFox SPI link not started");
    xTaskCreate(selfTestProducerTask, "video_selftest", 4096, nullptr, 3, nullptr);
  } else {
    luckfox_spi::init(onLuckfoxFrame, chain_node::setLaneStatus);
  }

  if (isGateway) {
    ESP_LOGI(kTag, "video_relay up: AP=%s (gateway, sink only)", apSsid);
  } else {
    ESP_LOGI(kTag, "video_relay up: AP=%s -> next=RAP-%u", apSsid, g_nextId);
  }
}

void logStats() {
  ESP_LOGI(kTag, "stats: prev=%s next=%s | local=%u in=%u out=%u (%u KB) drop=%u sunk=%u bad=%u | reconnects=%u prev_lost=%u | qlen=%u",
           g_serverUp ? "up" : "-", g_isGateway ? "n/a" : (g_clientUp ? "up" : "DOWN"), (unsigned)g_vLocal,
           (unsigned)g_vIn, (unsigned)g_vOut, (unsigned)(g_vOutBytes / 1024), (unsigned)g_vDrop, (unsigned)g_vSunk,
           (unsigned)g_vBad, (unsigned)g_vReconnects, (unsigned)g_vPrevDrops,
           (unsigned)(g_outQueue ? uxQueueMessagesWaiting(g_outQueue) : 0));
}

}  // namespace video_relay
