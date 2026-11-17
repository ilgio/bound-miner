#include "rpc/client.h"

#include "net/socket.h"
#include "rpc/http.h"
#include "util/base64.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace bound {

namespace {

// The parameters that are JSON values rather than strings.
struct Convert {
    std::string_view method;
    std::size_t index;
};
constexpr Convert kConvert[] = {
    {"getblockhash", 0},     {"getblock", 1},         {"getrawtransaction", 1}, {"getrawmempool", 0},
    {"generatetoaddress", 0}, {"listtransactions", 0}, {"sendtoaddress", 2},     {"bumpfee", 1},
    {"walletpassphrase", 1}, {"setban", 2},           {"gettxout", 1},          {"gettxout", 2},
    {"getaddressdeltas", 1}, {"getaddressdeltas", 2}, {"getaddressutxos", 1},   {"getaddressutxos", 2},
    {"getnetworkhashps", 0}, {"setmining", 0},        {"setmining", 2},         {"sendall", 1},
};

}  // namespace

json cli_param(std::string_view method, std::size_t index, const std::string& text) {
    for (const Convert& c : kConvert) {
        if (c.method != method || c.index != index) continue;
        json value = json::parse(text, nullptr, false);
        if (value.is_discarded() || !(value.is_number() || value.is_boolean() || value.is_null())) {
            throw std::invalid_argument("parameter " + std::to_string(index + 1) + " of " + std::string(method) +
                                        " must be a number or true/false, not " + text);
        }
        return value;
    }
    return text;
}

std::vector<std::string> split_words(std::string_view line) {
    std::vector<std::string> words;
    std::string word;
    bool in_word = false;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
            word += line[++i];
            in_word = true;
        } else if (c == '"') {
            quoted = !quoted;
            in_word = true;  // "" is an empty word
        } else if ((c == ' ' || c == '\t') && !quoted) {
            if (in_word) words.push_back(std::move(word));
            word.clear();
            in_word = false;
        } else {
            word += c;
            in_word = true;
        }
    }
    if (quoted) throw std::invalid_argument("a quote is left open");
    if (in_word) words.push_back(std::move(word));
    return words;
}

RpcClient::RpcClient(std::filesystem::path cookie_file, std::uint16_t port, int timeout_ms)
    : cookie_file_(std::move(cookie_file)), port_(port), timeout_ms_(timeout_ms) {}

std::string RpcClient::read_cookie() const {
    std::ifstream in(cookie_file_, std::ios::binary);
    if (!in) {
        throw std::runtime_error("can't read " + cookie_file_.string() + ": is `boundd node` running with this data directory?");
    }
    std::string cookie{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    while (!cookie.empty() && (cookie.back() == '\n' || cookie.back() == '\r' || cookie.back() == ' ')) cookie.pop_back();
    return cookie;
}

json RpcClient::call(const std::string& method, const json& params) {
    const std::string body = dump_json(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}});
    const NetAddress node = NetAddress::ipv4(127, 0, 0, 1, port_);
    const auto post = [&] {
        return http_post(node, "/", body, {{"Authorization", "Basic " + base64_encode(bytes_of(cookie_))}}, timeout_ms_);
    };
    if (cookie_.empty()) cookie_ = read_cookie();
    HttpResponse response = post();
    if (response.status == 401) {  // the node was restarted since we read its cookie
        cookie_ = read_cookie();
        response = post();
        if (response.status == 401) throw std::runtime_error("the node refused the cookie: is this its data directory?");
    }
    if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status) + " from the node: " + response.body);

    const json reply = json::parse(response.body, nullptr, false);
    if (reply.is_discarded() || !reply.is_object()) throw std::runtime_error("the node's reply isn't JSON-RPC");
    if (const auto error = reply.find("error"); error != reply.end() && !error->is_null()) {
        if (!error->is_object()) throw std::runtime_error("the node's reply isn't JSON-RPC");
        throw RpcError(error->value("code", rpc_error::kMisc), error->value("message", std::string()));
    }
    const auto result = reply.find("result");
    return result != reply.end() ? *result : json(nullptr);
}

}  // namespace bound
