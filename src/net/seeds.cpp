#include "net/seeds.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace bound {

bool SeedResolver::start(std::vector<std::string> names, std::uint16_t default_port) {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->running) return false;
        state_->running = true;
        state_->done = false;
        state_->found.clear();
    }
    std::thread([state = state_, names = std::move(names), default_port] {
        std::vector<NetAddress> found;
        for (const std::string& name : names) {
            for (const NetAddress& addr : resolve(name, default_port)) {
                if (std::find(found.begin(), found.end(), addr) == found.end()) found.push_back(addr);
            }
        }
        const std::lock_guard lock(state->mutex);
        state->found = std::move(found);
        state->running = false;
        state->done = true;
    }).detach();
    return true;
}

std::optional<std::vector<NetAddress>> SeedResolver::take() {
    const std::lock_guard lock(state_->mutex);
    if (!state_->done) return std::nullopt;
    state_->done = false;
    return std::move(state_->found);
}

}  // namespace bound
