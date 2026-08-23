#pragma once

#include <cstdint>

// One-time identity assignment for a node: which position (node_id) it
// occupies in the chain and how many nodes the chain has (chain_size). This
// is the only thing that still has to be told to each unit by hand --
// neighbor MAC addresses are learned automatically at runtime (see
// chain_node.h discovery phase).
namespace provisioning {

struct Identity {
  uint8_t node_id;
  uint8_t chain_size;
};

// Reads node_id/chain_size from NVS. If either is missing/invalid, blocks on
// the console UART (prints instructions, waits for a "SETID <id> <chain_size>"
// line, persists it, then restarts the device) -- so this never returns with
// an invalid Identity.
Identity loadOrProvision();

}  // namespace provisioning
