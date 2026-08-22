#pragma once

#include <cstdint>

// One-time identity assignment for a node: which position (node_id) it
// occupies in the ring and how big the ring is. This is the only thing that
// still has to be told to each unit by hand -- neighbor MAC addresses are
// learned automatically at runtime (see ring_node.h discovery phase).
namespace provisioning {

struct Identity {
  uint8_t node_id;
  uint8_t ring_size;
};

// Reads node_id/ring_size from NVS. If either is missing/invalid, blocks on
// the console UART (prints instructions, waits for a "SETID <id> <ring_size>"
// line, persists it, then restarts the device) -- so this never returns with
// an invalid Identity.
Identity loadOrProvision();

}  // namespace provisioning
