#include "net/banman.h"

#include "util/serialize.h"

#include <algorithm>

namespace bound {

namespace {

constexpr std::uint8_t kBanListVersion = 1;
constexpr std::size_t kMaxBans = 100'000;

bool active(const std::map<BanMan::Ip, std::int64_t>& list, const NetAddress& addr, std::int64_t now) {
    const auto found = list.find(addr.ip);
    return found != list.end() && now < found->second;
}

}  // namespace

void BanMan::discourage(const NetAddress& addr, std::int64_t now) {
    std::erase_if(discouraged_, [&](const auto& entry) { return entry.second <= now; });
    if (discouraged_.size() >= kMaxDiscouraged && !discouraged_.contains(addr.ip)) {
        discouraged_.erase(std::min_element(discouraged_.begin(), discouraged_.end(),
                                            [](const auto& a, const auto& b) { return a.second < b.second; }));
    }
    discouraged_[addr.ip] = now + kDiscourageTime;
}

bool BanMan::is_discouraged(const NetAddress& addr, std::int64_t now) const { return active(discouraged_, addr, now); }

void BanMan::ban(const NetAddress& addr, std::int64_t until) { banned_[addr.ip] = until; }

bool BanMan::unban(const NetAddress& addr) { return banned_.erase(addr.ip) > 0; }

bool BanMan::is_banned(const NetAddress& addr, std::int64_t now) const { return active(banned_, addr, now); }

std::vector<std::pair<NetAddress, std::int64_t>> BanMan::banned(std::int64_t now) {
    std::erase_if(banned_, [&](const auto& entry) { return entry.second <= now; });
    std::vector<std::pair<NetAddress, std::int64_t>> list;
    for (const auto& [ip, until] : banned_) {
        NetAddress addr;
        addr.ip = ip;
        list.emplace_back(addr, until);
    }
    return list;
}

Bytes BanMan::serialize() const {
    ByteWriter w;
    w.u8(kBanListVersion);
    w.compact_size(banned_.size());
    for (const auto& [ip, until] : banned_) {
        w.bytes(ip);
        w.i64(until);
    }
    return std::move(w).take();
}

BanMan BanMan::deserialize(ByteSpan data) {
    ByteReader r(data);
    if (r.u8() != kBanListVersion) throw DeserializeError("unknown banlist.dat version");
    const std::uint64_t count = r.compact_size();
    if (count > kMaxBans || count * 24 > r.remaining()) throw DeserializeError("banlist.dat count too large");
    BanMan banman;
    for (std::uint64_t i = 0; i < count; ++i) {
        Ip ip;
        const ByteSpan raw = r.bytes(16);
        std::copy(raw.begin(), raw.end(), ip.begin());
        banman.banned_[ip] = r.i64();
    }
    r.expect_end();
    return banman;
}

}  // namespace bound
