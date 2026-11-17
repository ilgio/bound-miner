#pragma once

#include "core/uint256.h"

#include <span>

namespace bound {

// BOUND's merkle tree: parent = tagged_hash("BOUND/merkle", left.bytes() || right.bytes()).
// A level with an odd count promotes its last node unchanged. (Bitcoin duplicates it instead,
// so [a, b, c] and [a, b, c, c] share a root: CVE-2012-2459.) One leaf: the root is that leaf.
// No leaves: zero.
uint256 merkle_root(std::span<const uint256> leaves);

}  // namespace bound
