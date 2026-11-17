// bound-cli: talks to a running boundd over JSON-RPC, like bitcoin-cli.
//   bound-cli [--mainnet | --testnet | --regtest] [--datadir <dir>] [--rpcport <port>] <method> [param]...
// It reads the node's cookie from the data directory (the same default as boundd's), so it works
// for whoever can read that directory. Parameters are passed as strings, except those the table
// in rpc/client.cpp marks as numbers or true/false. `bound-cli help` lists the methods.
#include "app/datadir.h"
#include "core/params.h"
#include "rpc/client.h"
#include "rpc/http.h"
#include "rpc/server.h"
#include "util/version.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bound;

int usage() {
    std::cerr << "usage: bound-cli [--mainnet | --testnet | --regtest] [--datadir <dir>] [--rpcport <port>] <method> [param]...\n"
                 "       bound-cli help             lists the methods\n"
                 "       bound-cli help <method>    explains one\n"
                 "       bound-cli --version\n"
                 "The network's node is asked ("
              << network_name(kDefaultNetwork) << " by default); its data directory defaults to " << default_datadir().string()
              << " for " << network_name(kDefaultNetwork) << ".\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (bound::asks_for_version(argc, argv)) {
        std::cout << bound::version_text("bound-cli");
        return 0;
    }
    std::uint16_t port = 0;
    try {
        std::vector<std::string_view> args(argv + 1, argv + argc);
        // The options come before the method: the network first among them (milestone 10).
        std::size_t options = 0;
        while (options < args.size() && args[options].starts_with("--")) options += network_flag(args[options]) ? 1 : 2;
        const Network network = take_network(args, options);
        port = params_for(network).default_rpc_port;
        std::filesystem::path datadir = default_datadir(network);
        while (args.size() >= 2 && (args[0] == "--datadir" || args[0] == "--rpcport")) {
            if (args[0] == "--datadir") {
                datadir = std::filesystem::path(args[1]);
            } else {
                port = static_cast<std::uint16_t>(std::stoul(std::string(args[1])));
            }
            args.erase(args.begin(), args.begin() + 2);
        }
        if (args.empty() || args[0].starts_with("-")) return usage();

        const std::string method(args[0]);
        json params = json::array();
        for (std::size_t i = 1; i < args.size(); ++i) params.push_back(cli_param(method, i - 1, std::string(args[i])));

        // 15 minutes: generatetoaddress and restorewallet can take a while.
        RpcClient node(cookie_path(datadir), port, 900'000);
        const json result = node.call(method, params);
        if (result.is_string()) {
            const std::string text = result.get<std::string>();
            std::cout << text << (text.ends_with('\n') ? "" : "\n");
        } else if (!result.is_null()) {
            std::cout << dump_json(result, 2) << "\n";
        }
        return 0;
    } catch (const RpcError& e) {
        std::cerr << "error code: " << e.code << "\nerror message:\n" << e.what() << "\n";
        return 1;
    } catch (const HttpConnectError&) {
        std::cerr << "bound-cli: can't connect to the node at 127.0.0.1:" << port << ": is `boundd node` running?\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "bound-cli: " << e.what() << "\n";
        return 1;
    }
}
