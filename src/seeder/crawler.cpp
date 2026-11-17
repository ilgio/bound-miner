#include "seeder/crawler.h"

#include "util/serialize.h"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <utility>

namespace bound {

namespace {

constexpr std::uint8_t kFormatVersion = 1;
constexpr std::int64_t kDay = 24 * 60 * 60;

std::int64_t retry_delay(std::uint32_t failures) { return std::min<std::int64_t>(std::int64_t{60} << std::min<std::uint32_t>(failures, 10), kDay); }

}  // namespace

Crawler::Crawler(CrawlerOptions options, std::uint64_t seed) : options_(std::move(options)), rng_(seed) {}

bool Crawler::usable(const NetAddress& addr) const {
    if (addr.port == 0 || addr.is_unspecified()) return false;
    return options_.allow_private || addr.is_routable();
}

bool Crawler::add(const NetAddress& addr, std::int64_t now) {
    if (!usable(addr)) return false;
    return add_start(addr, now);
}

bool Crawler::add_start(const NetAddress& addr, std::int64_t now) {
    if (addr.port == 0 || addr.is_unspecified() || nodes_.contains(addr) || nodes_.size() >= options_.max_nodes) return false;
    NodeRecord& r = nodes_[addr];
    r.addr = addr;
    r.first_seen = now;
    return true;
}

const NodeRecord* Crawler::find(const NetAddress& addr) const {
    const auto found = nodes_.find(addr);
    return found == nodes_.end() ? nullptr : &found->second;
}

void Crawler::tick(std::int64_t now) {
    // Probes that ran out of time: no answer to the connect or the handshake is a failure; an
    // answered handshake whose addr reply never came is still a success.
    for (auto& [id, probe] : probes_) {
        if (probe.done) continue;
        if (probe.asked_addr != 0) {
            if (now - probe.asked_addr > options_.addr_wait) finish(probe, true, now);
        } else if (now - probe.started > (probe.connected ? options_.probe_timeout : options_.connect_timeout)) {
            finish(probe, false, now);
        }
    }
    std::erase_if(probes_, [](const auto& entry) { return entry.second.done; });

    // Hopeless addresses: never answered in five tries, or not for two weeks and ten tries.
    if (now - last_cleanup_ >= 60) {
        last_cleanup_ = now;
        std::set<NetAddress> busy;
        for (const auto& [id, probe] : probes_) busy.insert(probe.addr);
        std::erase_if(nodes_, [&](const auto& entry) {
            const NodeRecord& r = entry.second;
            if (busy.contains(r.addr)) return false;
            return (r.successes == 0 && r.failures >= 5) || (r.failures >= 10 && now - r.last_success > 14 * kDay);
        });
    }

    // Start what's due: never tried first, then the longest since tried.
    if (probes_.size() >= options_.max_probes) return;
    std::set<NetAddress> busy;
    for (const auto& [id, probe] : probes_) busy.insert(probe.addr);
    std::vector<const NodeRecord*> due;
    for (const auto& [addr, r] : nodes_) {
        if (busy.contains(addr)) continue;
        if (r.tries == 0) {
            due.push_back(&r);
            continue;
        }
        const std::int64_t wait = r.failures == 0 ? options_.recheck : retry_delay(r.failures);
        if (now - r.last_try >= wait) due.push_back(&r);
    }
    std::sort(due.begin(), due.end(), [](const NodeRecord* a, const NodeRecord* b) {
        if ((a->tries == 0) != (b->tries == 0)) return a->tries == 0;
        return a->last_try < b->last_try;
    });
    for (const NodeRecord* r : due) {
        if (probes_.size() >= options_.max_probes) break;
        NodeRecord& record = nodes_.at(r->addr);
        record.last_try = now;
        Probe probe(options_.magic);
        probe.addr = record.addr;
        probe.started = now;
        try {
            probe.socket = Socket::connect(record.addr);
        } catch (const std::runtime_error&) {
            finish(probe, false, now);  // e.g. no route to an IPv6 address
            continue;
        }
        probes_.emplace(next_id_++, std::move(probe));
    }
}

void Crawler::send(Probe& probe, std::string_view command, ByteSpan payload) const {
    const Bytes frame = frame_message(options_.magic, command, payload);
    probe.out.insert(probe.out.end(), frame.begin(), frame.end());
}

void Crawler::add_poll_items(std::vector<PollItem>& items) {
    polled_.clear();
    for (const auto& [id, probe] : probes_) {
        if (probe.done) continue;
        items.push_back({probe.socket.handle(), probe.connected, !probe.connected || probe.out_pos < probe.out.size()});
        polled_.push_back(id);
    }
}

void Crawler::handle_poll(std::span<const PollItem> items, std::int64_t now) {
    for (std::size_t i = 0; i < polled_.size() && i < items.size(); ++i) {
        const auto found = probes_.find(polled_[i]);
        if (found == probes_.end() || found->second.done) continue;
        Probe& probe = found->second;
        const PollItem& item = items[i];
        if (!probe.connected) {
            if (!item.writable && !item.failed) continue;
            if (item.failed || probe.socket.connect_error() != 0) {
                finish(probe, false, now);
                continue;
            }
            probe.connected = true;
            VersionMsg v;
            v.services = 0;  // not a node: nothing to download from us
            v.time = now;
            v.addr_you = probe.addr;
            v.nonce = rng_() | 1;
            v.user_agent = options_.user_agent;
            send(probe, command::kVersion, encode_version(v));
        } else if (item.readable || item.failed) {
            read(probe, now);
        }
        if (!probe.done) flush(probe);
    }
    std::erase_if(probes_, [](const auto& entry) { return entry.second.done; });
}

void Crawler::flush(Probe& probe) {
    while (probe.out_pos < probe.out.size()) {
        const IoResult r = probe.socket.send(ByteSpan(probe.out).subspan(probe.out_pos));
        if (r.closed) {
            probe.done = true;  // the read side records how it went
            return;
        }
        if (r.would_block || r.bytes == 0) return;
        probe.out_pos += r.bytes;
    }
}

void Crawler::read(Probe& probe, std::int64_t now) {
    std::array<std::uint8_t, 16 << 10> buffer;
    for (std::size_t total = 0; total < (1 << 20) && !probe.done;) {
        const IoResult r = probe.socket.recv(buffer);
        if (r.would_block) return;
        if (r.closed) {
            finish(probe, probe.asked_addr != 0, now);  // hanging up after the handshake is fine
            return;
        }
        total += r.bytes;
        try {
            probe.reader.feed(ByteSpan(buffer.data(), r.bytes));
            while (!probe.done) {
                const std::optional<Message> m = probe.reader.next();
                if (!m) break;
                if (!handle(probe, *m, now)) finish(probe, false, now);
            }
        } catch (const ProtocolError&) {
            finish(probe, false, now);  // another network's node, or garbage
        }
    }
}

bool Crawler::handle(Probe& probe, const Message& m, std::int64_t now) {
    if (m.command == command::kVersion) {
        if (probe.got_version) return false;
        try {
            probe.version = decode_version(m.payload);
        } catch (const DeserializeError&) {
            return false;
        }
        probe.got_version = true;
        send(probe, command::kVerack, {});
    } else if (m.command == command::kVerack) {
        probe.got_verack = true;
    } else if (m.command == command::kPing) {
        send(probe, command::kPong, m.payload);
    } else if (m.command == command::kAddr && probe.asked_addr != 0) {
        try {
            for (const AddrEntry& entry : decode_addresses(m.payload)) add(entry.addr, now);
        } catch (const DeserializeError&) {
            // a bad list costs it nothing here: the handshake already answered
        }
        finish(probe, true, now);
        return true;
    }
    if (probe.got_version && probe.got_verack && probe.asked_addr == 0) {
        if (probe.version.version < kProtocolVersion || (probe.version.services & kNodeNetwork) == 0) return false;
        probe.asked_addr = now;
        send(probe, command::kGetAddr, {});
    }
    return true;
}

void Crawler::finish(Probe& probe, bool success, std::int64_t now) {
    probe.done = true;
    const auto found = nodes_.find(probe.addr);
    if (found == nodes_.end()) return;
    NodeRecord& r = found->second;
    r.last_try = std::max(r.last_try, probe.started);
    ++r.tries;
    const double result = success ? 1.0 : 0.0;
    r.reliability = r.tries == 1 ? result : 0.8 * r.reliability + 0.2 * result;
    if (success) {
        ++r.successes;
        r.failures = 0;
        r.last_success = now;
        r.version = probe.version.version;
        r.services = probe.version.services;
        r.height = probe.version.height;
        r.user_agent = probe.version.user_agent;
    } else {
        ++r.failures;
    }
}

std::vector<NetAddress> Crawler::good(std::int64_t now) const {
    std::vector<std::int32_t> heights;
    for (const auto& [addr, r] : nodes_) {
        if (r.last_success != 0 && now - r.last_success <= kDay) heights.push_back(r.height);
    }
    std::int32_t floor = 0;
    if (!heights.empty()) {
        std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2), heights.end());
        floor = heights[heights.size() / 2] - 1440;
    }
    std::vector<NetAddress> list;
    for (const auto& [addr, r] : nodes_) {
        if (addr.port != options_.default_port || !usable(addr)) continue;
        if (r.failures != 0 || r.last_success == 0 || now - r.last_success > 2 * options_.recheck + options_.probe_timeout) continue;
        if (r.reliability < 0.5 || r.version < kProtocolVersion || (r.services & kNodeNetwork) == 0) continue;
        if (r.height < floor) continue;
        list.push_back(addr);
    }
    return list;
}

Bytes Crawler::serialize() const {
    ByteWriter w;
    w.u8(kFormatVersion);
    w.compact_size(nodes_.size());
    for (const auto& [addr, r] : nodes_) {
        w.bytes(addr.ip);
        w.u16(addr.port);
        w.i64(r.first_seen);
        w.i64(r.last_try);
        w.i64(r.last_success);
        w.u32(r.failures);
        w.u32(r.tries);
        w.u32(r.successes);
        w.u32(static_cast<std::uint32_t>(std::clamp(r.reliability, 0.0, 1.0) * 1'000'000));
        w.u32(r.version);
        w.u64(r.services);
        w.i32(r.height);
        w.var_bytes(bytes_of(r.user_agent));
    }
    return std::move(w).take();
}

void Crawler::load(ByteSpan data) {
    ByteReader r(data);
    if (r.u8() != kFormatVersion) throw DeserializeError("unknown seeder.dat version");
    const std::uint64_t count = r.compact_size();
    constexpr std::size_t kMinRecord = 16 + 2 + 3 * 8 + 5 * 4 + 8 + 4 + 1;
    if (count > options_.max_nodes || count > r.remaining() / kMinRecord) throw DeserializeError("seeder.dat count too large");
    std::map<NetAddress, NodeRecord> nodes;
    for (std::uint64_t i = 0; i < count; ++i) {
        NodeRecord n;
        const ByteSpan ip = r.bytes(16);
        std::copy(ip.begin(), ip.end(), n.addr.ip.begin());
        n.addr.port = r.u16();
        n.first_seen = r.i64();
        n.last_try = r.i64();
        n.last_success = r.i64();
        n.failures = r.u32();
        n.tries = r.u32();
        n.successes = r.u32();
        n.reliability = r.u32() / 1'000'000.0;
        n.version = r.u32();
        n.services = r.u64();
        n.height = r.i32();
        const Bytes agent = r.var_bytes(kMaxUserAgent);
        n.user_agent.assign(agent.begin(), agent.end());
        if (usable(n.addr)) nodes[n.addr] = std::move(n);
    }
    r.expect_end();
    nodes_ = std::move(nodes);
}

}  // namespace bound
