#pragma once

#include "core/amount.h"
#include "rpc/http.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bound {

using json = nlohmann::json;

// Milestone 7: JSON-RPC 2.0 (bitcoind's interface, in its newer dialect). Parameters are
// positional. Amounts are strings with 8 decimals ("1.50000000"), never floating point; they may
// be given as strings or numbers.

// Error codes: JSON-RPC's own, then Bitcoin Core's (its rpc/protocol.h) where one fits.
namespace rpc_error {
inline constexpr int kParse = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kMisc = -1;
inline constexpr int kType = -3;
inline constexpr int kWallet = -4;
inline constexpr int kInvalidAddressOrKey = -5;
inline constexpr int kInsufficientFunds = -6;
inline constexpr int kInvalidParameter = -8;
inline constexpr int kKeypoolRanOut = -12;
inline constexpr int kUnlockNeeded = -13;
inline constexpr int kWrongPassphrase = -14;
inline constexpr int kNoWallet = -18;
inline constexpr int kDeserialization = -22;
inline constexpr int kVerifyRejected = -26;
}  // namespace rpc_error

struct RpcError : std::runtime_error {
    RpcError(int c, const std::string& message) : std::runtime_error(message), code(c) {}
    int code;
};

// A method's positional parameters. Every accessor throws RpcError(kType) on a wrong type; the
// table has already checked how many there are.
class RpcParams {
public:
    explicit RpcParams(const json& params) : params_(params) {}

    std::size_t size() const { return params_.is_array() ? params_.size() : 0; }
    bool has(std::size_t i) const { return i < size() && !params_[i].is_null(); }
    std::string str(std::size_t i) const;
    std::int64_t integer(std::size_t i) const;
    bool boolean(std::size_t i) const;
    // A positive amount of BOUND: "1.5" or 1.5. More than 8 decimals is an error, not rounded.
    Amount amount(std::size_t i) const;
    double number(std::size_t i) const;  // a plain number, e.g. a fee rate
    // How much detail: 0 to `max`, or false / true for 0 / 1 as older bitcoind takes it.
    // `fallback` when the parameter is missing.
    int verbosity(std::size_t i, int fallback, int max) const;

    std::optional<std::string> opt_str(std::size_t i) const { return has(i) ? std::optional(str(i)) : std::nullopt; }
    std::optional<std::int64_t> opt_integer(std::size_t i) const {
        return has(i) ? std::optional(integer(i)) : std::nullopt;
    }

private:
    const json& params_;
};

class RpcTable {
public:
    using Method = std::function<json(const RpcParams&)>;

    // `usage` lists the parameters ("<address> <amount> [fee_rate]"), `help` says what it does.
    void add(std::string name, std::string usage, std::string help, std::size_t min_params, std::size_t max_params,
             Method method);
    bool contains(const std::string& name) const { return methods_.contains(name); }

    // One request object to its response object. Never throws: a method's exception becomes an
    // error response (RpcError keeps its code; anything else is kMisc).
    json call(const json& request) const;
    // A whole HTTP body, one request or a batch (an array), to the response body.
    std::string handle(std::string_view body) const;

    std::string help() const;                         // every method, one line each
    std::string help(const std::string& name) const;  // one method; throws RpcError if unknown

private:
    struct Entry {
        std::string usage;
        std::string help;
        std::size_t min_params = 0;
        std::size_t max_params = 0;
        Method method;
    };
    std::map<std::string, Entry> methods_;
};

// JSON text that can't throw on bad UTF-8 (invalid bytes become U+FFFD).
std::string dump_json(const json& j, int indent = -1);

// The HTTP side of the RPC server (Bitcoin Core's httprpc.cpp): POST / with HTTP Basic auth whose
// "user:password" must equal `credentials`, compared in constant time; then the body goes to the
// table. Anything else gets 401, 404 or 405.
HttpResponse serve_json_rpc(const RpcTable& table, std::string_view credentials, const HttpRequest& request);

// Milestone 7's authentication, as in Bitcoin Core: at startup the node writes "__cookie__:" and 64
// random hex digits to <datadir>/.cookie, readable only by whoever can read the data directory,
// and bound-cli reads it from there.
inline constexpr std::string_view kCookieUser = "__cookie__";
std::string make_cookie();  // "__cookie__:<64 hex digits>"

}  // namespace bound
