// bound-seeder: a DNS seed for a BOUND network (the job of Bitcoin's DNS seeders); the default
// network (kDefaultNetwork) unless --mainnet, --testnet or --regtest says otherwise. One per
// machine: it answers DNS on port 53, which only one program can hold.
//   bound-seeder [--mainnet | --testnet | --regtest] --host <name> --ns <name> [--mbox <name>] [--dnsport <port>] [--dnsaddr <ip>]
//                [--node <host[:port]>]... [--datadir <dir>] [--probes <n>] [--allowprivate] [--quiet]
// It crawls the network, starting from the --node addresses (and whatever it remembered in
// <datadir>/seeder.dat), and answers DNS queries for --host with the addresses of nodes that are
// up. --ns is this machine's own name, as the parent zone delegates to it:
//   seed.example.org.  NS  ns.example.org.
//   ns.example.org.    A   <this machine's IP>
// DNS is port 53, which takes root or CAP_NET_BIND_SERVICE on Linux (see contrib/systemd).
#include "app/datadir.h"
#include "core/params.h"
#include "net/socket.h"
#include "seeder/crawler.h"
#include "seeder/dns.h"
#include "seeder/dns_server.h"
#include "util/serialize.h"
#include "util/time.h"
#include "util/version.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bound;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void log_line(const std::string& text) { std::cout << format_utc(unix_now()) << " " << text << "\n" << std::flush; }

int usage() {
    std::cerr << "usage: bound-seeder [--mainnet | --testnet | --regtest] --host <name> --ns <name> [--mbox <name>]\n"
                 "                    [--dnsport <port>] [--dnsaddr <ip>]\n"
                 "                    [--node <host[:port]>]... [--datadir <dir>] [--probes <n>] [--allowprivate]\n"
                 "                    [--quiet]\n"
                 "  --host     the seed's name, the one nodes look up: seed.example.org\n"
                 "  --ns       this machine's name, which the parent zone delegates the seed's name to\n"
                 "  --mbox     the operator's mailbox, DNS style (hostmaster.example.org); default hostmaster.<host>\n"
                 "  --dnsport  53 unless said otherwise; --dnsaddr binds one address instead of all\n"
                 "  --node     a node to start crawling from (the network's port unless one is given); the\n"
                 "             node on this machine, 127.0.0.1, works too, though only public addresses\n"
                 "             are ever handed out\n"
                 "  --datadir  where seeder.dat is kept; default <the network's data directory>/seeder, e.g. "
              << (default_datadir() / "seeder").string() << "\n";
    return 2;
}

std::uint16_t parse_port(std::string_view text) {
    const unsigned long port = std::stoul(std::string(text));
    if (port == 0 || port > 65535) throw std::invalid_argument("not a port: " + std::string(text));
    return static_cast<std::uint16_t>(port);
}

std::string dns_name(std::string_view text, const char* what) {
    const std::optional<std::string> name = normalize_dns_name(text);
    if (!name) throw std::invalid_argument(std::format("{} isn't a valid DNS name: {}", what, text));
    return *name;
}

// Written to a temporary file first, so a crash mid-write can't leave half a file behind.
void save(const std::filesystem::path& file, const Bytes& data) {
    const std::filesystem::path temp = file.string() + ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return;
    }
    std::filesystem::rename(temp, file);
}

}  // namespace

int main(int argc, char** argv) {
    if (bound::asks_for_version(argc, argv)) {
        std::cout << bound::version_text("bound-seeder");
        return 0;
    }
    try {
        std::vector<std::string_view> args(argv + 1, argv + argc);
        const Network network = take_network(args);  // milestone 10
        const ConsensusParams& params = params_for(network);
        std::optional<std::string> host;
        std::optional<std::string> ns;
        std::optional<std::string> mbox;
        std::uint16_t dns_port = 53;
        std::optional<NetAddress> dns_address;
        std::vector<std::string> nodes;
        std::filesystem::path datadir = default_datadir(network) / "seeder";
        CrawlerOptions crawl;
        crawl.magic = params.message_start;
        crawl.default_port = params.default_port;
        bool quiet = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const bool has_value = i + 1 < args.size();
            if (args[i] == "--host" && has_value) {
                host = dns_name(args[++i], "--host");
            } else if (args[i] == "--ns" && has_value) {
                ns = dns_name(args[++i], "--ns");
            } else if (args[i] == "--mbox" && has_value) {
                mbox = dns_name(args[++i], "--mbox");
            } else if (args[i] == "--dnsport" && has_value) {
                dns_port = parse_port(args[++i]);
            } else if (args[i] == "--dnsaddr" && has_value) {
                const std::vector<NetAddress> found = resolve(args[++i], 0);
                if (found.empty()) throw std::invalid_argument("not an IP address: " + std::string(args[i]));
                dns_address = found.front();
            } else if (args[i] == "--node" && has_value) {
                nodes.emplace_back(args[++i]);
            } else if (args[i] == "--datadir" && has_value) {
                datadir = std::filesystem::path(args[++i]);
            } else if (args[i] == "--probes" && has_value) {
                crawl.max_probes = static_cast<std::size_t>(std::stoul(std::string(args[++i])));
            } else if (args[i] == "--allowprivate") {
                crawl.allow_private = true;
            } else if (args[i] == "--quiet") {
                quiet = true;
            } else {
                return usage();
            }
        }
        if (!host || !ns) return usage();

        std::filesystem::create_directories(datadir);
        const std::filesystem::path state_file = datadir / "seeder.dat";
        std::random_device rd;
        Crawler crawler(crawl, std::uint64_t{rd()} << 32 | rd());
        if (std::ifstream in(state_file, std::ios::binary); in) {
            const Bytes data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            try {
                crawler.load(data);
            } catch (const DeserializeError&) {
                log_line("seeder.dat is damaged; starting from the --node addresses alone");
            }
        }
        const std::int64_t started = unix_now();
        for (const std::string& node : nodes) {
            const std::vector<NetAddress> found = resolve(node, params.default_port);
            if (found.empty()) log_line("can't resolve " + node);
            for (const NetAddress& addr : found) crawler.add_start(addr, started);
        }
        if (crawler.size() == 0) throw std::runtime_error("no addresses to start from: give one or more --node");

        DnsZone zone;
        zone.host = *host;
        zone.nameserver = *ns;
        zone.mailbox = mbox ? *mbox : "hostmaster." + *host;
        zone.serial = static_cast<std::uint32_t>(started);
        DnsServer dns(UdpSocket::bind(dns_port, dns_address), zone, [&] { return crawler.good(unix_now()); },
                      std::uint64_t{rd()} << 32 | rd());

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        log_line(std::format("BOUND seeder for {} (name server {}) on UDP port {}; {} addresses to start from", zone.host,
                             zone.nameserver, dns.port(), crawler.size()));
        std::int64_t last_report = started;
        std::int64_t last_save = started;
        while (!g_stop) {
            const std::int64_t now = unix_now();
            crawler.tick(now);
            std::vector<PollItem> items;
            crawler.add_poll_items(items);
            const std::size_t crawler_items = items.size();
            dns.add_poll_items(items);
            poll(items, 200);
            const std::int64_t after = unix_now();
            crawler.handle_poll(std::span<const PollItem>(items).first(crawler_items), after);
            dns.handle_poll(std::span<const PollItem>(items).subspan(crawler_items), after);

            if (!quiet && after - last_report >= 60) {
                last_report = after;
                const DnsServer::Stats& s = dns.stats();
                log_line(std::format("{} known, {} probing, {} good; {} queries answered, {} refused, {} dropped", crawler.size(),
                                     crawler.probing(), crawler.good(after).size(), s.queries, s.refused, s.dropped));
            }
            if (after - last_save >= 600) {
                last_save = after;
                save(state_file, crawler.serialize());
            }
        }
        save(state_file, crawler.serialize());
        log_line("stopped");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bound-seeder: " << e.what() << "\n";
        return 1;
    }
}
