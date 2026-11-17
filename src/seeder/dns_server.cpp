#include "seeder/dns_server.h"

#include <algorithm>
#include <array>
#include <utility>

namespace bound {

namespace {

constexpr double kBurst = 20;
constexpr double kPerSecond = 10;
constexpr std::size_t kMaxTracked = 100'000;     // addresses with a bucket; forgotten all at once beyond
constexpr int kMaxPacketsPerPass = 512;          // so a flood can't hold the loop

}  // namespace

DnsServer::DnsServer(UdpSocket socket, DnsZone zone, Addresses addresses, std::uint64_t seed)
    : socket_(std::move(socket)), zone_(std::move(zone)), addresses_(std::move(addresses)), rng_(seed) {}

void DnsServer::add_poll_items(std::vector<PollItem>& items) { items.push_back({socket_.handle(), true, false}); }

bool DnsServer::allow(const NetAddress& from, std::int64_t now) {
    if (buckets_.size() >= kMaxTracked) buckets_.clear();
    const auto [it, inserted] = buckets_.try_emplace(from.ip, Bucket{kBurst, now});
    Bucket& b = it->second;
    if (!inserted) {
        b.tokens = std::min(kBurst, b.tokens + kPerSecond * static_cast<double>(std::max<std::int64_t>(0, now - b.refilled)));
        b.refilled = now;
    }
    if (b.tokens < 1) return false;
    b.tokens -= 1;
    return true;
}

void DnsServer::handle_poll(std::span<const PollItem> items, std::int64_t now) {
    if (items.empty() || !(items[0].readable || items[0].failed)) return;
    std::array<std::uint8_t, 1500> packet;
    for (int n = 0; n < kMaxPacketsPerPass; ++n) {
        const auto received = socket_.receive(packet);
        if (!received) return;
        const auto& [size, from] = *received;
        if (!allow(from, now)) {
            ++stats_.dropped;
            continue;
        }
        const ByteSpan data(packet.data(), size);
        const std::optional<DnsQuery> query = parse_dns_query(data);
        if (!query) {
            // A query we can't read gets "format error", if it's a query at all; anything else, nothing.
            ++stats_.dropped;
            if (size >= 12 && (packet[2] & 0x80) == 0) {
                socket_.send(build_dns_error(static_cast<std::uint16_t>(packet[0] << 8 | packet[1]), dns::kFormErr), from);
            }
            continue;
        }
        if (now - cached_at_ >= 10) {
            cached_ = addresses_();
            cached_at_ = now;
        }
        std::shuffle(cached_.begin(), cached_.end(), rng_);  // a different sample each time
        const Bytes reply = build_dns_reply(*query, zone_, cached_);
        const bool ours = query->name == zone_.host || query->name.ends_with("." + zone_.host);
        ++(ours ? stats_.queries : stats_.refused);
        socket_.send(reply, from);
    }
}

}  // namespace bound
