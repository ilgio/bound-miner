#pragma once

#include "net/addrman.h"
#include "net/banman.h"
#include "net/peerman.h"
#include "net/seeds.h"
#include "net/socket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bound {

struct ConnmanOptions {
    bool listen = true;
    std::uint16_t port = 0;            // to listen on; 0 lets the OS pick (tests)
    bool loopback_only = false;        // listen on 127.0.0.1 only (tests)
    std::uint16_t default_port = 0;    // for --connect targets given without one
    std::vector<std::string> connect;  // --connect peers: kept connected, retried when they drop
    std::size_t max_outbound = 8;      // connections we open, --connect ones included
    std::size_t max_inbound = 32;
    // Milestone 8: inbound connections one IP may hold, so a single host can't take every slot.
    // Doesn't apply to this machine's own addresses (several local nodes share 127.0.0.1).
    std::size_t max_inbound_per_ip = 4;
    // Host names that lead to nodes (net/seeds.h). Asked at once when no addresses are known;
    // otherwise only if fewer than two outbound peers have turned up 11 seconds in (Core's
    // delay), and again every 10 minutes while that stays so.
    std::vector<std::string> seeds;
    std::int64_t connect_timeout = 5;  // seconds
};

// Milestone 6: the sockets (Bitcoin's CConnman). One thread runs a poll loop over the listening
// socket and every connection: it accepts peers, opens outbound connections (to the --connect
// peers first, then to addresses from the AddrMan), and moves bytes between the sockets and the
// PeerManager.
class Connman {
public:
    using Clock = std::function<std::int64_t()>;

    // Starts listening (if options.listen). Throws std::runtime_error if the port can't be had.
    // Banned and discouraged addresses (banman) are neither dialled nor let in; --connect peers
    // are dialled regardless.
    Connman(PeerManager& peers, AddrMan& addrman, BanMan& banman, ConnmanOptions options, PeerManager::Logger log,
            Clock clock);
    ~Connman();
    Connman(const Connman&) = delete;
    Connman& operator=(const Connman&) = delete;

    // The port listened on (the one the OS picked, after asking for 0); 0 if not listening.
    std::uint16_t port() const;
    // Milestone 7: polls the client's sockets in the same loop, on the same thread. It must outlive
    // run().
    void add_client(PollClient& client) { clients_.push_back(&client); }
    // Runs until stop(). `each_loop` runs on this thread on every pass, at least every 100 ms: the
    // place to hand over work for the chainstate, such as mined blocks.
    void run(const std::function<void()>& each_loop = {});
    void stop() { stopping_ = true; }  // from any thread

private:
    struct Connection {
        Socket socket;
        NetAddress addr;
        bool inbound = false;
        std::optional<std::size_t> manual;  // index into manual_ for --connect peers
    };
    struct Connecting {
        Socket socket;
        NetAddress addr;
        std::int64_t started = 0;
        std::optional<std::size_t> manual;
    };
    struct Manual {
        std::string target;
        std::int64_t next_try = 0;
        std::int64_t delay = 5;  // doubles after each failure, up to 5 minutes
        bool busy = false;       // connecting or connected
    };

    void use_seeds(std::int64_t now);
    void open_connections(std::int64_t now);
    void start_connect(const NetAddress& addr, std::optional<std::size_t> manual, std::int64_t now);
    void accept_all(std::int64_t now);
    bool read(PeerId id, Connection& c, std::int64_t now);
    bool write(PeerId id, Connection& c);
    void close(PeerId id);
    void log(const std::string& text) const;

    PeerManager& peers_;
    AddrMan& addrman_;
    BanMan& banman_;
    ConnmanOptions options_;
    PeerManager::Logger log_;
    Clock clock_;
    Socket listener_;
    std::map<PeerId, Connection> connections_;
    std::vector<Connecting> connecting_;
    std::vector<Manual> manual_;
    PeerId next_id_ = 1;
    std::atomic<bool> stopping_{false};
    std::vector<PollClient*> clients_;
    SeedResolver resolver_;
    std::int64_t started_ = 0;       // when run() began
    std::int64_t next_seeding_ = 0;  // not before this
};

}  // namespace bound
