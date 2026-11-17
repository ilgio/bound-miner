#include "core/merkle.h"

#include "crypto/sha256.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace bound {

uint256 merkle_root(std::span<const uint256> leaves) {
    if (leaves.empty()) return uint256{};
    std::vector<uint256> level(leaves.begin(), leaves.end());
    while (level.size() > 1) {
        std::vector<uint256> next;
        for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
            std::array<std::uint8_t, 64> pair;
            std::copy(level[i].bytes().begin(), level[i].bytes().end(), pair.begin());
            std::copy(level[i + 1].bytes().begin(), level[i + 1].bytes().end(), pair.begin() + 32);
            next.push_back(uint256(tagged_hash("BOUND/merkle", pair)));
        }
        if (level.size() % 2 == 1) next.push_back(level.back());
        level = std::move(next);
    }
    return level[0];
}

}  // namespace bound
