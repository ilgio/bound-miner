#pragma once

// What the programs agree on: which network (--mainnet, --testnet, --regtest), where its data
// directory is, and the RPC cookie in it.

#include "core/params.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bound {

inline std::optional<std::string> env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

// %APPDATA%\BOUND\<network> on Windows, ~/.bound/<network> elsewhere (testnet, mainnet, regtest).
inline std::filesystem::path default_datadir(Network network = kDefaultNetwork) {
    const std::string name(network_name(network));
#ifdef _WIN32
    if (const auto appdata = env("APPDATA")) return std::filesystem::path(*appdata) / "BOUND" / name;
#else
    if (const auto home = env("HOME")) return std::filesystem::path(*home) / ".bound" / name;
#endif
    return std::filesystem::path("bound-data") / name;
}

inline std::filesystem::path cookie_path(const std::filesystem::path& datadir) { return datadir / ".cookie"; }

// Milestone 10: takes --mainnet, --testnet and --regtest out of `args` (among the first `scan` of
// them; all by default) and returns that network, or kDefaultNetwork if none is there. Throws
// std::invalid_argument if two different ones are given.
inline Network take_network(std::vector<std::string_view>& args, std::size_t scan = SIZE_MAX) {
    std::optional<Network> chosen;
    for (std::size_t i = 0; i < args.size() && i < scan;) {
        const std::optional<Network> n = network_flag(args[i]);
        if (!n) {
            ++i;
            continue;
        }
        if (chosen && *chosen != *n) throw std::invalid_argument("choose one network: --mainnet, --testnet or --regtest");
        chosen = n;
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        if (scan != SIZE_MAX) --scan;
    }
    return chosen.value_or(kDefaultNetwork);
}

}  // namespace bound
