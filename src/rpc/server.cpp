#include "rpc/server.h"

#include "util/base64.h"
#include "util/hex.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace bound {

namespace {

json error_reply(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"error", {{"code", code}, {"message", message}}}, {"id", id}};
}

const json& param(const json& params, std::size_t i) {
    if (!params.is_array() || i >= params.size()) throw RpcError(rpc_error::kInvalidParams, std::format("missing parameter {}", i + 1));
    return params[i];
}

}  // namespace

std::string dump_json(const json& j, int indent) { return j.dump(indent, ' ', false, json::error_handler_t::replace); }

std::string RpcParams::str(std::size_t i) const {
    const json& p = param(params_, i);
    if (!p.is_string()) throw RpcError(rpc_error::kType, std::format("parameter {} must be a string", i + 1));
    return p.get<std::string>();
}

std::int64_t RpcParams::integer(std::size_t i) const {
    const json& p = param(params_, i);
    if (!p.is_number_integer()) throw RpcError(rpc_error::kType, std::format("parameter {} must be an integer", i + 1));
    return p.get<std::int64_t>();
}

bool RpcParams::boolean(std::size_t i) const {
    const json& p = param(params_, i);
    if (!p.is_boolean()) throw RpcError(rpc_error::kType, std::format("parameter {} must be true or false", i + 1));
    return p.get<bool>();
}

int RpcParams::verbosity(std::size_t i, int fallback, int max) const {
    if (!has(i)) return fallback;
    const json& p = param(params_, i);
    if (p.is_boolean()) return p.get<bool>() ? 1 : 0;
    if (p.is_number_integer()) {
        const std::int64_t v = p.get<std::int64_t>();
        if (v >= 0 && v <= max) return static_cast<int>(v);
    }
    throw RpcError(rpc_error::kType, std::format("parameter {} must be a verbosity from 0 to {}", i + 1, max));
}

double RpcParams::number(std::size_t i) const {
    const json& p = param(params_, i);
    if (p.is_number()) return p.get<double>();
    if (p.is_string()) {
        try {
            std::size_t used = 0;
            const std::string s = p.get<std::string>();
            const double v = std::stod(s, &used);
            if (used == s.size() && std::isfinite(v)) return v;
        } catch (const std::exception&) {
            // reported below
        }
    }
    throw RpcError(rpc_error::kType, std::format("parameter {} must be a number", i + 1));
}

Amount RpcParams::amount(std::size_t i) const {
    const json& p = param(params_, i);
    std::optional<Amount> a;
    if (p.is_string()) {
        a = parse_coins(p.get<std::string>());
    } else if (p.is_number_integer()) {
        const std::int64_t whole = p.get<std::int64_t>();
        if (whole >= 0 && whole <= kMaxMoney / kCoin) a = whole * kCoin;
    } else if (p.is_number()) {
        // A JSON number is a double here. In base units it must be a whole number, give or take
        // the double's own rounding (a couple of ulps), or it had more than 8 decimals.
        const double v = p.get<double>();
        if (std::isfinite(v) && v >= 0 && v <= static_cast<double>(kMaxMoney / kCoin)) {
            const double units = v * static_cast<double>(kCoin);
            if (std::fabs(units - std::round(units)) <= std::max(1e-3, units * 4e-16)) a = static_cast<Amount>(std::llround(units));
        }
    }
    if (!a || *a <= 0) throw RpcError(rpc_error::kType, std::format("parameter {} must be a positive amount, at most 8 decimals", i + 1));
    return *a;
}

void RpcTable::add(std::string name, std::string usage, std::string help, std::size_t min_params, std::size_t max_params,
                   Method method) {
    methods_[std::move(name)] = Entry{std::move(usage), std::move(help), min_params, max_params, std::move(method)};
}

json RpcTable::call(const json& request) const {
    json id = nullptr;
    try {
        if (!request.is_object()) throw RpcError(rpc_error::kInvalidRequest, "a request must be a JSON object");
        if (const auto found = request.find("id"); found != request.end()) id = *found;
        const auto method = request.find("method");
        if (method == request.end() || !method->is_string()) throw RpcError(rpc_error::kInvalidRequest, "the request has no method");
        const std::string name = method->get<std::string>();
        const auto entry = methods_.find(name);
        if (entry == methods_.end()) throw RpcError(rpc_error::kMethodNotFound, "unknown method: " + name);

        const json params = request.contains("params") ? request["params"] : json(nullptr);
        if (!params.is_null() && !params.is_array()) throw RpcError(rpc_error::kInvalidParams, "parameters must be a list (named ones aren't supported)");
        const RpcParams p(params);
        if (p.size() < entry->second.min_params || p.size() > entry->second.max_params) {
            throw RpcError(rpc_error::kInvalidParams, "usage: " + name + (entry->second.usage.empty() ? "" : " " + entry->second.usage));
        }
        return {{"jsonrpc", "2.0"}, {"result", entry->second.method(p)}, {"id", id}};
    } catch (const RpcError& e) {
        return error_reply(id, e.code, e.what());
    } catch (const std::exception& e) {
        return error_reply(id, rpc_error::kMisc, e.what());
    }
}

std::string RpcTable::handle(std::string_view body) const {
    const json request = json::parse(body, nullptr, false);
    if (request.is_discarded()) return dump_json(error_reply(nullptr, rpc_error::kParse, "the request isn't valid JSON"));
    if (request.is_array()) {
        if (request.empty()) return dump_json(error_reply(nullptr, rpc_error::kInvalidRequest, "an empty batch"));
        json replies = json::array();
        for (const json& r : request) replies.push_back(call(r));
        return dump_json(replies);
    }
    return dump_json(call(request));
}

std::string RpcTable::help() const {
    std::string text;
    for (const auto& [name, entry] : methods_) text += name + (entry.usage.empty() ? "" : " " + entry.usage) + "\n";
    return text;
}

std::string RpcTable::help(const std::string& name) const {
    const auto entry = methods_.find(name);
    if (entry == methods_.end()) throw RpcError(rpc_error::kMethodNotFound, "unknown method: " + name);
    return name + (entry->second.usage.empty() ? "" : " " + entry->second.usage) + "\n\n" + entry->second.help + "\n";
}

HttpResponse serve_json_rpc(const RpcTable& table, std::string_view credentials, const HttpRequest& request) {
    bool authorized = false;
    if (const std::optional<std::string> auth = request.header("authorization"); auth && auth->starts_with("Basic ")) {
        const std::optional<Bytes> given = base64_decode(std::string_view(*auth).substr(6));
        authorized = given && given->size() == credentials.size() &&
                     sodium_memcmp(given->data(), credentials.data(), credentials.size()) == 0;
    }
    if (!authorized) return {401, "", "text/plain", {{"WWW-Authenticate", "Basic realm=\"jsonrpc\""}}};
    if (request.target != "/") return {404, "not found\n", "text/plain", {}};
    if (request.method != "POST") return {405, "JSON-RPC takes POST\n", "text/plain", {}};
    return {200, table.handle(request.body), "application/json", {}};
}

std::string make_cookie() {
    if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    std::array<std::uint8_t, 32> secret;
    randombytes_buf(secret.data(), secret.size());
    return std::string(kCookieUser) + ":" + to_hex(secret);
}

}  // namespace bound
