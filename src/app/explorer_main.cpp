// bound-explorer: a block explorer for a BOUND node, as web pages.
//   bound-explorer [--mainnet | --testnet | --regtest] [--datadir <dir>] [--rpcport <port>] [--port <port>]
//                  [--public] [--title <text>] [--quiet]
// It asks the node on this machine over JSON-RPC (with the cookie in the node's data directory)
// and serves the pages on http://127.0.0.1:<port> (8080 unless told otherwise); with --public, on
// every interface, for the world to read. Run the node with --index, or transactions already in
// blocks and address pages can't be shown.
#include "app/datadir.h"
#include "core/params.h"
#include "explorer/explorer.h"
#include "net/socket.h"
#include "rpc/client.h"
#include "rpc/http.h"
#include "util/time.h"
#include "util/version.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bound;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void log_line(const std::string& text) { std::cout << format_utc(unix_now()) << " " << text << "\n" << std::flush; }

// A request's target as it's safe to print: what a client sends ends up in the operator's terminal.
std::string printable(std::string_view text) {
    std::string out(text.substr(0, 120));
    for (char& c : out) {
        if (c < 0x20 || c > 0x7e) c = '?';
    }
    return out;
}

int usage() {
    std::cerr << "usage: bound-explorer [--mainnet | --testnet | --regtest] [--datadir <dir>] [--rpcport <port>]\n"
                 "                      [--port <port>] [--public] [--title <text>] [--staleafter <seconds>] [--quiet]\n"
                 "Serves a block explorer for the BOUND node on this machine at http://127.0.0.1:<port>\n"
                 "(default 8080); --public serves it on every interface. The node's data directory\n"
                 "defaults to the network's ("
              << network_name(kDefaultNetwork) << "'s: " << default_datadir().string() << ").\n"
                 "/health answers 503 for uptime monitors when the node is down, has no peers, or has had\n"
                 "no block for --staleafter seconds (default: 12 blocks' time, at least an hour).\n";
    return 2;
}

std::uint16_t parse_port(std::string_view text) {
    const unsigned long port = std::stoul(std::string(text));
    if (port == 0 || port > 65535) throw std::invalid_argument("not a port: " + std::string(text));
    return static_cast<std::uint16_t>(port);
}

}  // namespace

int main(int argc, char** argv) {
    if (bound::asks_for_version(argc, argv)) {
        std::cout << bound::version_text("bound-explorer");
        return 0;
    }
    try {
        std::vector<std::string_view> args(argv + 1, argv + argc);
        const Network network = take_network(args);  // milestone 10
        std::filesystem::path datadir = default_datadir(network);
        std::uint16_t rpc_port = params_for(network).default_rpc_port;
        std::uint16_t port = 8080;
        bool open_to_all = false;
        bool quiet = false;
        ExplorerOptions options;
        options.network = std::string(network_name(network));
        options.title = network == Network::Mainnet ? "BOUND explorer" : "BOUND " + options.network + " explorer";
        options.stale_after = default_stale_after(params_for(network).target_spacing);  // milestone 14
        for (std::size_t i = 0; i < args.size(); ++i) {
            const bool has_value = i + 1 < args.size();
            if (args[i] == "--datadir" && has_value) {
                datadir = std::filesystem::path(args[++i]);
            } else if (args[i] == "--rpcport" && has_value) {
                rpc_port = parse_port(args[++i]);
            } else if (args[i] == "--port" && has_value) {
                port = parse_port(args[++i]);
            } else if (args[i] == "--title" && has_value) {
                options.title = std::string(args[++i]);
            } else if (args[i] == "--staleafter" && has_value) {
                options.stale_after = std::stoll(std::string(args[++i]));
                if (options.stale_after <= 0) throw std::invalid_argument("--staleafter takes a number of seconds");
            } else if (args[i] == "--public") {
                open_to_all = true;
            } else if (args[i] == "--quiet") {
                quiet = true;
            } else {
                return usage();
            }
        }

        RpcClient node(cookie_path(datadir), rpc_port, 10'000);
        Explorer explorer([&](const std::string& method, const json& params) { return node.call(method, params); }, unix_now, options);

        // Limits for a server strangers reach: a connection must state its request within 10
        // seconds and may idle for 15, and one address gets 16 of the 256.
        HttpServerOptions http;
        http.port = port;
        http.loopback_only = !open_to_all;
        http.max_connections = 256;
        http.max_per_ip = 16;
        http.idle_timeout = 15;
        http.request_timeout = 10;
        HttpServer server(http, [&](const HttpRequest& request) {
            const auto start = std::chrono::steady_clock::now();
            HttpResponse response = explorer.handle(request);
            if (!quiet) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                log_line(std::format("{} {} {} -> {} ({} ms)", request.peer.ip_string(), printable(request.method), printable(request.target),
                                     response.status, ms));
            }
            return response;
        });

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        log_line(std::format("BOUND explorer on http://{}:{}, asking the node at 127.0.0.1:{}", open_to_all ? "0.0.0.0" : "127.0.0.1",
                             server.port(), rpc_port));
        while (!g_stop) {
            std::vector<PollItem> items;
            server.add_poll_items(items);
            poll(items, 200);
            server.handle_poll(items, unix_now());
        }
        log_line("stopped");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bound-explorer: " << e.what() << "\n";
        return 1;
    }
}
