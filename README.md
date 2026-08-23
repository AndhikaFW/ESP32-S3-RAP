# ESP32-S3-RAP

Firmware for a chain of ESP32-S3 nodes, one per parking lot, each paired
with a LuckFox Pico Mini doing on-device camera AI (motion -> car detection
-> occupancy -> plate OCR). Every node in the chain is a parking lot -- there
is no separate "entrance"/gate node in this project (may be added later as
its own thing). Nodes talk to each other over ESP-NOW (small occupancy/plate
readings) and a separate WiFi link (bulk video); see
[`docs/network/topology.md`](docs/network/topology.md) for the full
write-up. "RAP" (Ring Access Protocol) is the project's original/historical
name from when this was a closed ring -- the topology is now a one-way
chain (all internal naming, e.g. `chain_node.*`, `chain_size`, was updated
to match), kept only as the repo/project name for continuity.

One node in the chain (`node_id == 0`, the **Gateway**) additionally carries
an ENC28J60 module and bridges the chain to a wired LAN over RJ45 to a
Raspberry Pi 4 backend -- it is still a parking-lot node like any other
(same firmware, same LuckFox pairing), just the one that also has this extra
uplink role. Every node runs the exact same firmware image; role and chain
position come from a small per-unit provisioning step, not from separate
builds.

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

### Building for a board without working PSRAM

If a specific unit's physical PSRAM chip is defective or absent (this
firmware degrades gracefully -- see `allocFrameBuffer()` in
`main/video_relay.cpp`), build it into a separate directory with PSRAM
disabled via `sdkconfig.nopsram.defaults` instead of flashing the normal
PSRAM-on image:

```sh
idf.py -B build_nopsram -D SDKCONFIG=build_nopsram/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.nopsram.defaults" build
idf.py -B build_nopsram -D SDKCONFIG=build_nopsram/sdkconfig -p PORT flash
```

`-D SDKCONFIG=build_nopsram/sdkconfig` matters: without it, idf.py writes/reads the merged
config at the project root (shared with the normal `build/`), so building this variant
after the normal one would silently reuse the PSRAM-on config instead of regenerating from
`sdkconfig.nopsram.defaults` (defaults are only applied when the target sdkconfig file
doesn't exist yet).

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
Send:  SETID <node_id> <chain_size>
```

Example, node 2 of a 5-node chain: `SETID 2 5`. The device saves this to NVS
and restarts. `node_id 0` is always the Gateway; everything else is a relay
(and, like the Gateway, still a parking-lot node with its own LuckFox).
Neighbor MAC addresses are **not** part of provisioning -- each node
discovers its next/prev neighbor automatically over ESP-NOW broadcast at
boot (see `main/chain_node.cpp`), so the same firmware binary can be flashed
to every unit.

## Layout

This submodule is ESP32-S3 firmware only -- the Raspberry Pi 4 backend/demo
listener scripts live in the parent repo's `backend/` folder, not in here
(see the root [`README.md`](../../README.md)).

- `docs/network/topology.md`, `topology.dot/.png/.svg` -- topology diagram and protocol write-up.
- `main/config.h` -- timing constants, pins, NVS keys.
- `main/protocol.h` -- ESP-NOW wire structs (`HelloPacket`, `StatusPacket`, `AckPacket`).
- `main/espnow_transport.*` -- ESP-NOW bring-up and RX queue.
- `main/provisioning.*` -- NVS identity + console provisioning prompt.
- `main/chain_node.*` -- discovery + chain state machine (every node originates its own `StatusPacket` and relays what it receives from `prev`; Gateway only sinks).
- `main/video_relay.*` -- separate WiFi AP+STA daisy-chain + TCP for bulk video (3 streams per node), independent of ESP-NOW.
- `main/gateway_uplink.*` -- ENC28J60 bring-up (via `espressif/enc28j60`) and both backend uplinks (status/plate + video, Gateway only).
- `main/main.cpp` -- `app_main`, wires the above together.
