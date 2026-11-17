#include "net/addrman.h"

#include "util/serialize.h"

#include <algorithm>
#include <climits>
#include <iterator>
#include <utility>

namespace bound {

namespace {

constexpr std::uint8_t kFormatVersion = 1;
constexpr std::size_t kEntrySize = 16 + 2 + 3 * 8 + 4;

std::int64_t retry_delay(int attempts) {
    constexpr std::int64_t kDay = 24 * 60 * 60;
    return std::min<std::int64_t>(std::int64_t{60} << std::min(attempts, 11), kDay);
}

}  // namespace

AddrMan::AddrMan(std::uint64_t seed) : rng_(seed) {}

void AddrMan::set_routable_only(bool on) {
    routable_only_ = on;
    if (on) std::erase_if(addrs_, [](const auto& entry) { return !entry.first.is_routable(); });
}

bool AddrMan::is_terrible(const AddrInfo& info, std::int64_t now) {
    constexpr std::int64_t kDay = 24 * 60 * 60;
    if (now - info.last_seen > 30 * kDay) return true;
    if (info.last_success == 0 && info.attempts >= 3) return true;
    return info.attempts >= 10 && now - info.last_success > 7 * kDay;
}

bool AddrMan::add(const NetAddress& addr, std::int64_t seen) {
    if (addr.is_unspecified() || addr.port == 0) return false;
    if (routable_only_ && !addr.is_routable()) return false;
    if (const auto found = addrs_.find(addr); found != addrs_.end()) {
        found->second.last_seen = std::max(found->second.last_seen, seen);
        return false;
    }
    if (addrs_.size() >= kMaxSize) {
        // Never-reached before proven, then the one seen longest ago.
        const auto victim = std::min_element(addrs_.begin(), addrs_.end(), [](const auto& a, const auto& b) {
            const bool a_proven = a.second.last_success != 0;
            const bool b_proven = b.second.last_success != 0;
            if (a_proven != b_proven) return b_proven;
            return a.second.last_seen < b.second.last_seen;
        });
        addrs_.erase(victim);
    }
    addrs_.emplace(addr, AddrInfo{addr, seen});
    return true;
}

void AddrMan::attempt(const NetAddress& addr, std::int64_t now) {
    if (const auto found = addrs_.find(addr); found != addrs_.end()) {
        found->second.last_try = now;
        ++found->second.attempts;
    }
}

void AddrMan::good(const NetAddress& addr, std::int64_t now) {
    if (const auto found = addrs_.find(addr); found != addrs_.end()) {
        found->second.last_success = now;
        found->second.last_seen = std::max(found->second.last_seen, now);
        found->second.attempts = 0;
    }
}

void AddrMan::remove(const NetAddress& addr) { addrs_.erase(addr); }

std::optional<NetAddress> AddrMan::select(const std::set<NetAddress>& exclude, std::int64_t now) {
    std::erase_if(addrs_, [&](const auto& entry) { return !exclude.contains(entry.first) && is_terrible(entry.second, now); });
    std::vector<const AddrInfo*> proven;
    std::vector<const AddrInfo*> untried;
    for (const auto& [addr, info] : addrs_) {
        if (exclude.contains(addr)) continue;
        if (info.last_try != 0 && now - info.last_try < retry_delay(info.attempts)) continue;
        (info.last_success != 0 ? proven : untried).push_back(&info);
    }
    if (proven.empty() && untried.empty()) return std::nullopt;
    const bool use_proven = untried.empty() || (!proven.empty() && rng_() % 2 == 0);
    const std::vector<const AddrInfo*>& from = use_proven ? proven : untried;
    std::uniform_int_distribution<std::size_t> pick(0, from.size() - 1);
    return from[pick(rng_)]->addr;
}

std::vector<AddrInfo> AddrMan::sample(std::size_t n, std::int64_t now) {
    std::vector<AddrInfo> all;
    all.reserve(addrs_.size());
    for (const auto& [addr, info] : addrs_) {
        if (now == 0 || !is_terrible(info, now)) all.push_back(info);
    }
    std::shuffle(all.begin(), all.end(), rng_);
    if (all.size() > n) all.resize(n);
    return all;
}

const AddrInfo* AddrMan::find(const NetAddress& addr) const {
    const auto found = addrs_.find(addr);
    return found == addrs_.end() ? nullptr : &found->second;
}

Bytes AddrMan::serialize() const {
    ByteWriter w;
    w.u8(kFormatVersion);
    w.compact_size(addrs_.size());
    for (const auto& [addr, info] : addrs_) {
        w.bytes(addr.ip);
        w.u16(addr.port);
        w.i64(info.last_seen);
        w.i64(info.last_try);
        w.i64(info.last_success);
        w.u32(static_cast<std::uint32_t>(info.attempts));
    }
    return std::move(w).take();
}

AddrMan AddrMan::deserialize(ByteSpan data, std::uint64_t seed) {
    ByteReader r(data);
    if (r.u8() != kFormatVersion) throw DeserializeError("unknown peers.dat version");
    const std::uint64_t n = r.compact_size();
    if (n > kMaxSize || n > r.remaining() / kEntrySize) throw DeserializeError("bad peers.dat entry count");
    AddrMan man(seed);
    for (std::uint64_t i = 0; i < n; ++i) {
        AddrInfo info;
        const ByteSpan ip = r.bytes(info.addr.ip.size());
        std::copy(ip.begin(), ip.end(), info.addr.ip.begin());
        info.addr.port = r.u16();
        info.last_seen = r.i64();
        info.last_try = r.i64();
        info.last_success = r.i64();
        const std::uint32_t attempts = r.u32();
        info.attempts = static_cast<int>(std::min<std::uint32_t>(attempts, INT_MAX));
        man.addrs_.emplace(info.addr, info);
    }
    r.expect_end();
    return man;
}

}  // namespace bound
