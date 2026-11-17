#pragma once

#include "rpc/http.h"
#include "rpc/server.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace bound {

// Milestone 8: a block explorer. It holds no chain data of its own: every page is built from a
// node's answers to JSON-RPC calls, and rendered here as plain HTML (no scripts). It runs as its
// own program, bound-explorer, so the public's web traffic never shares a thread, a port or a
// crash with the node.
//
//   /                    the chain at a glance and the latest blocks
//   /block/<hash|height> a block and its transactions
//   /tx/<txid>           a transaction: where it is, what it spent, what spent it
//   /address/<address>   an address's balance and history (needs the node's --index)
//   /mempool             what's waiting for a block
//   /search?q=...        a height, block hash, txid or address: redirects to its page
//   /health              for uptime monitors (milestone 14): JSON, 200 when the node answers,
//                        has peers and has a fresh block; 503 and the reason otherwise
//
// Everything that comes from the node or from the URL is HTML-escaped on its way into a page (a
// transaction's memo is whatever its sender typed), and pages carry a Content-Security-Policy
// that forbids scripts outright.

// One JSON-RPC call to the node: the result, or RpcError for an error reply, or any other
// exception if the node can't be reached.
using RpcCall = std::function<json(const std::string& method, const json& params)>;

struct ExplorerOptions {
    std::string network = "testnet";  // milestone 10: on every network but mainnet, coins have no value
    std::string title = "BOUND testnet explorer";
    int home_blocks = 15;           // blocks listed on the front page
    std::size_t page_size = 25;     // rows per page of an address's history
    std::size_t max_rows = 500;     // rows of a block's or the mempool's transactions shown
    std::int64_t cache_seconds = 2;  // a page is built at most once in this long...
    std::size_t cache_pages = 256;   // ...for this many different URLs
    std::int64_t stale_after = 3600;  // /health: the newest block is overdue after this long
};

// /health's limit for a network with this block spacing: twelve blocks' time, and never under an
// hour. Twelve blocks' time with no block happens by chance about once in 160,000 blocks.
std::int64_t default_stale_after(std::int64_t block_spacing);

class Explorer {
public:
    Explorer(RpcCall rpc, std::function<std::int64_t()> clock, ExplorerOptions options = {});

    // The response to one request. Never throws: a node that's down or a bad URL gets a page
    // saying so.
    HttpResponse handle(const HttpRequest& request);

private:
    struct Page {
        int status = 200;
        std::string title;
        std::string body;      // HTML for <main>
        std::string redirect;  // or: go there instead
    };

    Page route(std::string_view path, const std::map<std::string, std::string>& query);
    Page home();
    Page block(std::string_view id);
    Page transaction(std::string_view txid);
    Page address(std::string_view address, std::size_t page);
    Page mempool();
    Page search(std::string_view text);
    Page message(int status, std::string title, const std::string& text) const;
    HttpResponse health(std::int64_t now) const;
    HttpResponse render(const Page& page) const;
    json call(const std::string& method, const json& params = json::array()) const { return rpc_(method, params); }

    RpcCall rpc_;
    std::function<std::int64_t()> clock_;
    ExplorerOptions options_;
    struct Cached {
        HttpResponse response;
        std::int64_t time = 0;
    };
    std::map<std::string, Cached> cache_;
};

// & < > " ' as entities: safe in element content and in quoted attributes.
std::string html_escape(std::string_view text);
// %XX and + decoded. nullopt on a malformed escape.
std::optional<std::string> url_decode(std::string_view text);
// "a=1&b=two" as a map; later duplicates win, undecodable pairs are skipped.
std::map<std::string, std::string> parse_query(std::string_view query);

}  // namespace bound
