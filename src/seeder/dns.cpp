#include "seeder/dns.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace bound {

namespace {

void put16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put32(Bytes& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v >> 16));
    put16(out, static_cast<std::uint16_t>(v));
}

std::uint16_t get16(ByteSpan p, std::size_t at) { return static_cast<std::uint16_t>(p[at] << 8 | p[at + 1]); }

// A dotted name in wire form: each label after its length, then a zero. Precondition: a name
// normalize_dns_name accepts (or empty, for the root).
Bytes encode_name(std::string_view name) {
    Bytes out;
    while (!name.empty()) {
        const std::size_t dot = name.find('.');
        const std::string_view label = name.substr(0, dot);
        out.push_back(static_cast<std::uint8_t>(label.size()));
        out.insert(out.end(), label.begin(), label.end());
        if (dot == std::string_view::npos) break;
        name.remove_prefix(dot + 1);
    }
    out.push_back(0);
    return out;
}

// QR (a reply), the opcode, AA (we're the authority for the zone), RD as asked, and the result.
Bytes reply_header(std::uint16_t id, std::uint8_t opcode, bool recursion_desired, std::uint8_t rcode, std::uint16_t questions,
                   std::uint16_t answers, std::uint16_t authority, std::uint16_t additional) {
    Bytes out;
    put16(out, id);
    put16(out, static_cast<std::uint16_t>(0x8000 | (opcode & 0xf) << 11 | 0x0400 | (recursion_desired ? 0x0100 : 0) | (rcode & 0xf)));
    put16(out, questions);
    put16(out, answers);
    put16(out, authority);
    put16(out, additional);
    return out;
}

// A record whose name is the question's: a compression pointer to offset 12, where the
// question's name starts.
Bytes record_at_question(std::uint16_t type, std::uint32_t ttl, ByteSpan rdata) {
    Bytes out;
    put16(out, 0xC00C);
    put16(out, type);
    put16(out, dns::kClassIN);
    put32(out, ttl);
    put16(out, static_cast<std::uint16_t>(rdata.size()));
    out.insert(out.end(), rdata.begin(), rdata.end());
    return out;
}

Bytes soa_rdata(const DnsZone& zone) {
    Bytes out = encode_name(zone.nameserver);
    const Bytes mailbox = encode_name(zone.mailbox);
    out.insert(out.end(), mailbox.begin(), mailbox.end());
    put32(out, zone.serial);
    put32(out, 3600);    // refresh
    put32(out, 600);     // retry
    put32(out, 604800);  // expire
    put32(out, 60);      // how long a "no such name" may be cached
    return out;
}

// The zone's SOA record, named in full (the question may be for a name under the zone).
Bytes soa_record(const DnsZone& zone) {
    Bytes out = encode_name(zone.host);
    put16(out, dns::kTypeSOA);
    put16(out, dns::kClassIN);
    put32(out, zone.zone_ttl);
    const Bytes rdata = soa_rdata(zone);
    put16(out, static_cast<std::uint16_t>(rdata.size()));
    out.insert(out.end(), rdata.begin(), rdata.end());
    return out;
}

}  // namespace

std::optional<std::string> normalize_dns_name(std::string_view name) {
    if (!name.empty() && name.back() == '.') name.remove_suffix(1);
    if (name.empty() || name.size() > 253) return std::nullopt;
    std::string out;
    std::size_t label = 0;
    for (const char c : name) {
        if (c == '.') {
            if (label == 0) return std::nullopt;
            label = 0;
            out += '.';
            continue;
        }
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) == 0 && c != '-' && c != '_') return std::nullopt;
        if (++label > 63) return std::nullopt;
        out += static_cast<char>(std::tolower(u));
    }
    if (label == 0) return std::nullopt;
    return out;
}

std::optional<DnsQuery> parse_dns_query(ByteSpan p) {
    if (p.size() < 12) return std::nullopt;
    DnsQuery q;
    q.id = get16(p, 0);
    const std::uint16_t flags = get16(p, 2);
    if ((flags & 0x8000) != 0) return std::nullopt;  // a reply, not a query
    q.opcode = static_cast<std::uint8_t>(flags >> 11 & 0xf);
    q.recursion_desired = (flags & 0x0100) != 0;
    if (get16(p, 4) != 1) return std::nullopt;  // exactly one question, as every resolver sends
    const std::uint16_t answers = get16(p, 6);
    const std::uint16_t authority = get16(p, 8);
    const std::uint16_t additional = get16(p, 10);

    std::size_t at = 12;
    std::string name;
    while (true) {
        if (at >= p.size()) return std::nullopt;
        const std::uint8_t length = p[at];
        if (length == 0) {
            ++at;
            break;
        }
        if (length > 63) return std::nullopt;  // a compression pointer, or a reserved label type
        if (at + 1 + length > p.size()) return std::nullopt;
        if (!name.empty()) name += '.';
        for (std::size_t i = 0; i < length; ++i) name += static_cast<char>(std::tolower(p[at + 1 + i]));
        at += 1 + length;
        if (at - 12 > 255) return std::nullopt;
    }
    q.raw_name.assign(p.begin() + 12, p.begin() + static_cast<std::ptrdiff_t>(at));
    q.name = std::move(name);
    if (at + 4 > p.size()) return std::nullopt;
    q.type = get16(p, at);
    q.klass = get16(p, at + 2);
    at += 4;

    // EDNS: an OPT pseudo-record (root name, type 41, its class the UDP size the client takes).
    if (answers == 0 && authority == 0 && additional >= 1 && at + 11 <= p.size() && p[at] == 0 && get16(p, at + 1) == dns::kTypeOPT) {
        q.edns = true;
        q.udp_size = std::max<std::uint16_t>(512, get16(p, at + 3));
    }
    return q;
}

Bytes build_dns_error(std::uint16_t id, std::uint8_t rcode) { return reply_header(id, 0, false, rcode, 0, 0, 0, 0); }

Bytes build_dns_reply(const DnsQuery& q, const DnsZone& zone, std::span<const NetAddress> addresses) {
    const std::size_t limit = q.edns ? std::min<std::size_t>(q.udp_size, dns::kMaxReply) : dns::kMaxPlainReply;

    Bytes question = q.raw_name;
    put16(question, q.type);
    put16(question, q.klass);

    std::uint8_t rcode = dns::kNoError;
    std::vector<Bytes> answers;
    std::vector<Bytes> authority;
    const bool internet = q.klass == dns::kClassIN || q.klass == dns::kClassANY;
    if (q.opcode != 0) {
        rcode = dns::kNotImp;
    } else if (!internet) {
        rcode = dns::kRefused;
    } else if (q.name == zone.host) {
        switch (q.type) {
        case dns::kTypeA:
        case dns::kTypeANY:  // answered with the A records alone (RFC 8482 allows a subset)
            for (const NetAddress& a : addresses) {
                if (a.is_ipv4()) answers.push_back(record_at_question(dns::kTypeA, zone.ttl, ByteSpan(a.ip).subspan(12)));
            }
            break;
        case dns::kTypeAAAA:
            for (const NetAddress& a : addresses) {
                if (!a.is_ipv4()) answers.push_back(record_at_question(dns::kTypeAAAA, zone.ttl, a.ip));
            }
            break;
        case dns::kTypeNS:
            answers.push_back(record_at_question(dns::kTypeNS, zone.zone_ttl, encode_name(zone.nameserver)));
            break;
        case dns::kTypeSOA:
            answers.push_back(record_at_question(dns::kTypeSOA, zone.zone_ttl, soa_rdata(zone)));
            break;
        default:
            break;
        }
        // The name exists, but has nothing of that type ("no data"): say so with the SOA.
        if (answers.empty()) authority.push_back(soa_record(zone));
    } else if (q.name.size() > zone.host.size() && q.name.ends_with("." + zone.host)) {
        rcode = dns::kNXDomain;
        authority.push_back(soa_record(zone));
    } else {
        rcode = dns::kRefused;  // not our zone: we're nobody's resolver
    }

    // As many answers as fit. Never the "truncated" flag: a resolver would retry over TCP, which
    // this server doesn't speak, and a seed's answers are a random sample anyway.
    std::size_t size = 12 + question.size() + (q.edns ? 11 : 0);
    for (const Bytes& r : authority) size += r.size();
    std::size_t fitting = 0;
    for (const Bytes& r : answers) {
        if (size + r.size() > limit) break;
        size += r.size();
        ++fitting;
    }
    Bytes out = reply_header(q.id, q.opcode, q.recursion_desired, rcode, 1, static_cast<std::uint16_t>(fitting),
                             static_cast<std::uint16_t>(authority.size()), q.edns ? 1 : 0);
    out.insert(out.end(), question.begin(), question.end());
    for (std::size_t i = 0; i < fitting; ++i) out.insert(out.end(), answers[i].begin(), answers[i].end());
    for (const Bytes& r : authority) out.insert(out.end(), r.begin(), r.end());
    if (q.edns) {  // our own OPT record: the reply size we take, no flags
        out.push_back(0);
        put16(out, dns::kTypeOPT);
        put16(out, static_cast<std::uint16_t>(dns::kMaxReply));
        put32(out, 0);
        put16(out, 0);
    }
    return out;
}

}  // namespace bound
