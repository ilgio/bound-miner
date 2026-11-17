#include "core/chain.h"

#include <algorithm>

namespace bound {

const BlockIndex* BlockIndex::ancestor(int h) const {
    const BlockIndex* node = this;
    while (node->height > h) node = node->parent;
    return node;
}

std::uint32_t BlockIndex::median_time_past(int span) const {
    std::vector<std::uint32_t> times;
    for (const BlockIndex* node = this; node != nullptr && static_cast<int>(times.size()) < span; node = node->parent) {
        times.push_back(node->header.time);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

std::vector<uint256> block_locator(const BlockIndex& tip) {
    std::vector<uint256> hashes;
    int step = 1;
    for (const BlockIndex* node = &tip;; node = node->ancestor(std::max(0, node->height - step))) {
        hashes.push_back(node->hash);
        if (node->height == 0) return hashes;
        if (hashes.size() >= 10) step *= 2;
    }
}

}  // namespace bound
