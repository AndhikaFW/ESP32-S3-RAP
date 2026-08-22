# ESP32-S3-RAP

Ring Access Protocol firmware for a ring of ESP32-S3 nodes communicating over
ESP-NOW. One node in the ring (`node_id == 0`, the **Gateway**) also carries
an ENC28J60 module and bridges the ring to a wired LAN over RJ45. Every node
runs the exact same firmware image; role and ring position come from a small
per-unit provisioning step, not from separate builds.

Built on native **ESP-IDF** (no Arduino framework).

Full protocol/topology write-up (diagram, flow-control rules, state
machine): see [`docs/network/topology.md`](docs/network/topology.md).

## Build

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) (v5.0+; developed against v6.0.2). Activate the environment first, e.g.:

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh   # if installed via the EIM installer
# or: . $IDF_PATH/export.sh                         # if installed by cloning esp-idf directly
```

Then, from this directory:

```sh
idf.py set-target esp32s3   # only needed once per clean checkout
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

The `espressif/enc28j60` component is pulled automatically by the IDF
Component Manager on first build (declared in `main/idf_component.yml`).

### Flashing without pressing BOOT/RESET

ESP32-S3-DevKitC-1 (and most other devkits) exposes **two** USB-C ports:

- **"USB"** -- native USB straight into the ESP32-S3 (shows up as an
  Espressif VID/PID CDC device). Auto-reset over this port depends on
  whatever firmware is currently running exposing its own USB-CDC console,
  which is not reliable for unattended flashing.
- **"UART"** -- goes through an onboard USB-to-serial bridge chip (CP2102N
  or CH343, depending on board revision) wired to `EN`/`IO0` through a
  standard two-transistor auto-reset circuit. This is the port `esptool`
  auto-reset (and therefore `idf.py flash`) is designed for -- **use this
  port** and flashing needs no manual button presses at all.

## Provisioning a node

On first boot (or after `idf.py erase-flash`), a node has no identity yet
and blocks on the console UART:

```
=== ESP32-S3-RAP: node not provisioned ===
Send:  SETID <node_id> <ring_size>
```

Example, node 2 of a 5-node ring: `SETID 2 5`. The device saves this to NVS
and restarts. `node_id 0` is always the Gateway; everything else is a relay.
Neighbor MAC addresses are **not** part of provisioning -- each node
discovers its next/prev neighbor automatically over ESP-NOW broadcast at
boot (see `main/ring_node.cpp`), so the same firmware binary can be flashed
to every unit.

## Layout

- `docs/network/topology.md`, `topology.dot/.png/.svg` -- topology diagram and protocol write-up.
- `main/config.h` -- timing constants, pins, NVS keys.
- `main/protocol.h` -- wire structs (`HelloPacket`, `TokenPacket`, `AckPacket`).
- `main/espnow_transport.*` -- ESP-NOW bring-up and RX queue.
- `main/provisioning.*` -- NVS identity + console provisioning prompt.
- `main/ring_node.*` -- discovery + single-token ring state machine.
- `main/gateway_uplink.*` -- ENC28J60 bring-up (via `espressif/enc28j60`) and backend uplink (Gateway only).
- `main/main.cpp` -- `app_main`, wires the above together.
