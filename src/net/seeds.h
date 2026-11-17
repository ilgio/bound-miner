#pragma once

#include "net/socket.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bound {

// Milestone 8: how a node with nobody to ask finds its first peers. A seed is a host name (or an
// address) that leads to nodes: a DNS name whose records list reachable nodes, as bound-seeder
// serves (Bitcoin's DNS seeds), or simply the name of a node that's always up. The network's
// parameters list some, and --seednode adds more.
//
// Looking names up can take many seconds when DNS is slow or down, so it happens on a thread of
// its own and the node's loop collects the answer when it's ready.
class SeedResolver {
public:
    // Starts looking up `names` ("host" or "host:port"; default_port otherwise). False, and
    // nothing happens, if the last lookup is still running.
    bool start(std::vector<std::string> names, std::uint16_t default_port);
    // The addresses found, once the lookup has finished; nullopt while it's running, when there
    // was none, or when its result was already taken.
    std::optional<std::vector<NetAddress>> take();

private:
    struct State {
        std::mutex mutex;
        bool running = false;
        bool done = false;
        std::vector<NetAddress> found;
    };
    // Shared with the lookup thread, which may outlive this object (a lookup can't be cancelled).
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace bound
