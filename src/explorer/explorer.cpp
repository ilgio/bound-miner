#include "explorer/explorer.h"

#include "core/amount.h"
#include "util/time.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>
#include <vector>

namespace bound {

namespace {

constexpr std::string_view kStyle =
    ":root{color-scheme:light dark;--fg:#1a1d21;--bg:#fff;--muted:#667085;--line:#e4e7ec;--accent:#175cd3;--good:#067647;--bad:#b42318}"
    "@media(prefers-color-scheme:dark){:root{--fg:#e6e8eb;--bg:#101418;--muted:#98a2b3;--line:#2a313a;--accent:#84adff;--good:#47cd89;--bad:#f97066}}"
    "body{margin:0;font:15px/1.5 system-ui,sans-serif;color:var(--fg);background:var(--bg)}"
    "header,main,footer{max-width:1100px;margin:0 auto;padding:12px 16px}"
    "header{display:flex;gap:16px;align-items:center;flex-wrap:wrap;border-bottom:1px solid var(--line)}"
    "header a.brand{font-weight:700;color:var(--fg);font-size:18px}"
    "header form{margin-left:auto;display:flex;gap:6px;flex:1 1 320px}"
    "input[type=text]{flex:1;min-width:0;padding:7px 10px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}"
    "button{padding:7px 12px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit;cursor:pointer}"
    "a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}"
    "h1{font-size:20px;margin:18px 0 10px}h2{font-size:16px;margin:22px 0 8px}"
    ".wide{overflow-x:auto}table{border-collapse:collapse;width:100%}"
    "th,td{text-align:left;padding:6px 12px 6px 0;border-bottom:1px solid var(--line);vertical-align:top}"
    "th{color:var(--muted);font-weight:500;white-space:nowrap}"
    ".mono{font-family:ui-monospace,Consolas,monospace;font-size:13px;word-break:break-all}"
    ".num{text-align:right;white-space:nowrap}.muted{color:var(--muted)}.good{color:var(--good)}.bad{color:var(--bad)}"
    ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin:14px 0}"
    ".card{border:1px solid var(--line);border-radius:8px;padding:10px 12px}.card span{color:var(--muted);font-size:13px}.card b{display:block;font-size:17px}"
    ".kv th{width:190px}.pager{margin:14px 0;display:flex;gap:14px}"
    "footer{color:var(--muted);font-size:13px;border-top:1px solid var(--line);margin-top:28px}details{margin:14px 0}";

std::string esc(std::string_view text) { return html_escape(text); }

// What the URL may name: nothing else reaches the node.
bool is_height(std::string_view s) {
    return !s.empty() && s.size() <= 9 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}
bool is_hash(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
bool is_address_like(std::string_view s) {
    return s.size() >= 26 && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

// Fields of the node's replies, read without trusting their shape: a missing or mistyped one
// becomes a blank or a zero on the page rather than an error.
std::string text(const json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto found = j.find(key);
    return found != j.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::int64_t integer(const json& j, const char* key, std::int64_t fallback = 0) {
    if (!j.is_object()) return fallback;
    const auto found = j.find(key);
    return found != j.end() && found->is_number_integer() ? found->get<std::int64_t>() : fallback;
}

double real(const json& j, const char* key) {
    if (!j.is_object()) return 0;
    const auto found = j.find(key);
    return found != j.end() && found->is_number() ? found->get<double>() : 0.0;
}

bool flag(const json& j, const char* key) {
    if (!j.is_object()) return false;
    const auto found = j.find(key);
    return found != j.end() && found->is_boolean() && found->get<bool>();
}

const json& list(const json& j, const char* key) {
    static const json empty = json::array();
    if (!j.is_object()) return empty;
    const auto found = j.find(key);
    return found != j.end() && found->is_array() ? *found : empty;
}

std::string number(std::int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    for (std::size_t i = digits.size(); i > 3; i -= 3) digits.insert(i - 3, ",");
    return (n < 0 ? "-" : "") + digits;
}

std::string ago(std::int64_t seconds) {
    if (seconds < 0) return "ahead of this clock";
    if (seconds < 60) return std::format("{} s ago", seconds);
    if (seconds < 3600) return std::format("{} min ago", seconds / 60);
    if (seconds < 2 * 86400) return std::format("{} h ago", seconds / 3600);
    return std::format("{} days ago", seconds / 86400);
}

std::string when(std::int64_t time, std::int64_t now) {
    return format_utc(time) + " UTC <span class=\"muted\">(" + ago(now - time) + ")</span>";
}

std::string hash_rate(double per_second) {
    static constexpr const char* units[] = {"H/s", "kH/s", "MH/s", "GH/s", "TH/s", "PH/s"};
    std::size_t unit = 0;
    while (per_second >= 1000 && unit + 1 < std::size(units)) {
        per_second /= 1000;
        ++unit;
    }
    return std::format("{:.2f} {}", per_second, units[unit]);
}

std::string size_text(std::int64_t bytes) {
    return bytes < 10'000 ? number(bytes) + " bytes" : std::format("{:.1f} kB", static_cast<double>(bytes) / 1000);
}

std::string coins(const std::string& amount) { return esc(amount) + " BOUND"; }

std::string link(const std::string& href, const std::string& html) { return "<a href=\"" + esc(href) + "\">" + html + "</a>"; }
std::string mono(const std::string& html) { return "<span class=\"mono\">" + html + "</span>"; }
std::string tx_link(const std::string& txid) { return mono(link("/tx/" + txid, esc(txid))); }
std::string address_link(const std::string& address) { return mono(link("/address/" + address, esc(address))); }
std::string row(const std::string& name, const std::string& html) { return "<tr><th>" + name + "</th><td>" + html + "</td></tr>"; }
std::string card(const std::string& name, const std::string& html) {
    return "<div class=\"card\"><span>" + name + "</span><b>" + html + "</b></div>";
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

std::optional<std::string> url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%') {
            if (i + 2 >= text.size()) return std::nullopt;
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out += static_cast<char>(hi << 4 | lo);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(std::string_view query) {
    std::map<std::string, std::string> values;
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const std::size_t eq = pair.find('=');
        const std::optional<std::string> name = url_decode(pair.substr(0, eq));
        const std::optional<std::string> value = url_decode(eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1));
        if (name && value && !name->empty()) values[*name] = *value;
    }
    return values;
}

Explorer::Explorer(RpcCall rpc, std::function<std::int64_t()> clock, ExplorerOptions options)
    : rpc_(std::move(rpc)), clock_(std::move(clock)), options_(std::move(options)) {}

HttpResponse Explorer::handle(const HttpRequest& request) {
    // HEAD is GET without the body, which the HTTP server leaves off.
    if (request.method != "GET" && request.method != "HEAD") {
        return {405, "This server only answers GET and HEAD.\n", "text/plain; charset=utf-8", {{"Allow", "GET, HEAD"}}};
    }
    const std::string_view target = request.target;
    if (target == "/robots.txt") return {200, "User-agent: *\nDisallow: /search\n", "text/plain; charset=utf-8", {}};
    if (target.empty() || target.front() != '/' || target.size() > 300) {
        return render(message(404, "Not found", "There's no such page here."));
    }

    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    const std::map<std::string, std::string> query =
        question == std::string_view::npos ? std::map<std::string, std::string>{} : parse_query(target.substr(question + 1));

    // Each page is built at most once every cache_seconds, however many ask for it. The key is the
    // path and the two parameters that mean something, so "?x=1", "?x=2", ... all get one page.
    const std::int64_t now = clock_();
    std::string key(path);
    for (const char* name : {"q", "page"}) {
        if (const auto found = query.find(name); found != query.end()) key += std::string("?") + name + "=" + found->second;
    }
    if (const auto hit = cache_.find(key); hit != cache_.end() && now - hit->second.time < options_.cache_seconds) {
        return hit->second.response;
    }
    HttpResponse response;
    if (path == "/health") {
        response = health(now);
    } else {
        Page page;
        try {
            page = route(path, query);
        } catch (const RpcError& e) {
            if (e.code == rpc_error::kInvalidAddressOrKey || e.code == rpc_error::kInvalidParameter) {
                page = message(404, "Not found", "The node knows of nothing by that name.");
            } else {
                page = message(502, "The node refused", e.what());
            }
        } catch (const std::exception&) {
            page = message(503, "The node isn't answering",
                           "This explorer shows what its BOUND node tells it, and the node isn't answering right now. Try again in a moment.");
        }
        response = render(page);
    }
    if (response.status == 200 || response.status == 404) {
        if (cache_.size() >= options_.cache_pages) cache_.clear();
        cache_[key] = Cached{response, now};
    }
    return response;
}

std::int64_t default_stale_after(std::int64_t block_spacing) { return std::max<std::int64_t>(3600, 12 * block_spacing); }

// Milestone 14: what an uptime monitor polls. Healthy means the node answers, has at least one
// peer and its newest block isn't overdue; anything else is a 503 with the reason, which most
// monitors put in their alert.
HttpResponse Explorer::health(std::int64_t now) const {
    json report = {{"network", options_.network}};
    std::string problem;
    try {
        const json chain = call("getblockchaininfo");
        const json network = call("getnetworkinfo");
        const std::int64_t tip_time = chain.value("time", std::int64_t{0});
        const std::int64_t peers = network.value("connections", std::int64_t{0});
        report["height"] = chain.value("blocks", std::int64_t{0});
        report["tip_age"] = now - tip_time;
        report["peers"] = peers;
        if (peers == 0) {
            problem = "the node has no peers";
        } else if (now - tip_time > options_.stale_after) {
            problem = std::format("no block for {} minutes", (now - tip_time) / 60);
        }
    } catch (const std::exception&) {
        problem = "the node isn't answering";
    }
    report["status"] = problem.empty() ? "ok" : "unhealthy";
    if (!problem.empty()) report["problem"] = problem;
    return {problem.empty() ? 200 : 503, report.dump() + "\n", "application/json", {{"Cache-Control", "no-store"}}};
}

Explorer::Page Explorer::route(std::string_view path, const std::map<std::string, std::string>& query) {
    const auto parameter = [&](const char* name) {
        const auto found = query.find(name);
        return found == query.end() ? std::string() : found->second;
    };
    if (path == "/") return home();
    if (path == "/mempool") return mempool();
    if (path == "/search") return search(parameter("q"));
    if (path.starts_with("/block/")) return block(path.substr(7));
    if (path.starts_with("/tx/")) return transaction(path.substr(4));
    if (path.starts_with("/address/")) {
        const std::string page = parameter("page");
        return address(path.substr(9), is_height(page) && page.size() <= 6 ? static_cast<std::size_t>(std::stoul(page)) : 1);
    }
    return message(404, "Not found", "There's no such page here.");
}

Explorer::Page Explorer::message(int status, std::string title, const std::string& text) const {
    std::string body = "<h1>" + esc(title) + "</h1><p>" + esc(text) + "</p>";
    return {status, std::move(title), std::move(body), {}};
}

HttpResponse Explorer::render(const Page& page) const {
    std::vector<std::pair<std::string, std::string>> headers = {
        // No scripts, no frames, nothing loaded from anywhere: the pages are text and one inline style sheet.
        {"Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
    };
    if (!page.redirect.empty()) {
        headers.emplace_back("Location", page.redirect);
        return {302, "", "text/plain; charset=utf-8", std::move(headers)};
    }
    headers.emplace_back("Cache-Control", std::format("public, max-age={}", options_.cache_seconds));
    std::string html = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>";
    html += esc(page.title) + " - " + esc(options_.title) + "</title><style>";
    html += kStyle;
    html += "</style></head><body><header><a class=\"brand\" href=\"/\">" + esc(options_.title) +
            "</a><a href=\"/\">Blocks</a><a href=\"/mempool\">Mempool</a>"
            "<form action=\"/search\" method=\"get\"><input type=\"text\" name=\"q\" maxlength=\"80\" "
            "placeholder=\"Block height or hash, transaction id, or address\" aria-label=\"Search\">"
            "<button type=\"submit\">Search</button></form></header><main>";
    html += page.body;
    html += "</main><footer>";
    if (options_.network != "mainnet") html += "A " + esc(options_.network) + ": these coins have no value. ";
    html += "Pages show what one BOUND node sees.</footer></body></html>";
    return {page.status, std::move(html), "text/html; charset=utf-8", std::move(headers)};
}

Explorer::Page Explorer::home() {
    const std::int64_t now = clock_();
    const json info = call("getblockchaininfo");
    const json pool = call("getmempoolinfo");
    const json net = call("getnetworkinfo");
    const json rate = call("getnetworkhashps");

    std::string b = "<h1>" + esc(options_.title) + "</h1><div class=\"cards\">";
    b += card("Height", number(integer(info, "blocks")));
    b += card("Difficulty", std::format("{:.2f}", real(info, "difficulty")));
    b += card("Network hash rate", hash_rate(rate.is_number() ? rate.get<double>() : 0.0));
    b += card("Coins mined", coins(text(info, "supply")));
    b += card("Waiting transactions", link("/mempool", number(integer(pool, "size"))));
    b += card("This node's connections", number(integer(net, "connections")));
    b += "</div><h2>Latest blocks</h2><div class=\"wide\"><table><tr><th>Height</th><th>Hash</th><th>Mined</th>"
         "<th class=\"num\">Transactions</th><th class=\"num\">Size</th></tr>";
    std::string hash = text(info, "bestblockhash");
    for (int i = 0; i < options_.home_blocks && is_hash(hash); ++i) {
        const json block = call("getblock", json::array({hash, 1}));
        b += "<tr><td>" + link("/block/" + hash, number(integer(block, "height"))) + "</td><td>" + mono(link("/block/" + hash, esc(hash))) +
             "</td><td>" + when(integer(block, "time"), now) + "</td><td class=\"num\">" +
             number(static_cast<std::int64_t>(list(block, "tx").size())) + "</td><td class=\"num\">" + size_text(integer(block, "size")) +
             "</td></tr>";
        hash = text(block, "previousblockhash");
    }
    b += "</table></div>";
    return {200, "Blocks", std::move(b), {}};
}

Explorer::Page Explorer::block(std::string_view id) {
    std::string hash(id);
    if (is_height(id)) {
        const json found = call("getblockhash", json::array({std::stoll(hash)}));
        hash = found.is_string() ? found.get<std::string>() : std::string();
    }
    if (!is_hash(hash)) return message(404, "Not found", "That isn't a block height or a block hash.");
    const json block = call("getblock", json::array({hash, 2}));
    const std::int64_t now = clock_();
    const std::int64_t height = integer(block, "height");
    const std::int64_t confirmations = integer(block, "confirmations", -1);
    const json& txs = list(block, "tx");

    std::string b = "<h1>Block " + number(height) + "</h1><div class=\"wide\"><table class=\"kv\">";
    b += row("Hash", mono(esc(hash)));
    b += row("Confirmations", confirmations > 0 ? number(confirmations) : "<span class=\"bad\">Not on the main chain</span>");
    b += row("Mined", when(integer(block, "time"), now));
    b += row("Transactions", number(static_cast<std::int64_t>(txs.size())));
    b += row("Size", size_text(integer(block, "size")));
    b += row("Difficulty", std::format("{:.2f}", real(block, "difficulty")));
    b += row("Bits", mono(esc(text(block, "bits"))));
    b += row("Nonce", number(integer(block, "nonce")));
    b += row("Merkle root", mono(esc(text(block, "merkleroot"))));
    b += row("Chain work", mono(esc(text(block, "chainwork"))));
    if (const std::string previous = text(block, "previousblockhash"); is_hash(previous)) {
        b += row("Previous block", mono(link("/block/" + previous, esc(previous))));
    }
    if (const std::string next = text(block, "nextblockhash"); is_hash(next)) {
        b += row("Next block", mono(link("/block/" + next, esc(next))));
    }
    b += "</table></div><h2>Transactions</h2><div class=\"wide\"><table><tr><th>#</th><th>Transaction</th><th class=\"num\">Inputs</th>"
         "<th class=\"num\">Outputs</th><th class=\"num\">Output total</th><th class=\"num\">Fee</th></tr>";
    std::size_t shown = 0;
    for (const json& tx : txs) {
        if (shown == options_.max_rows) break;
        Amount total = 0;
        for (const json& out : list(tx, "vout")) total += parse_coins(text(out, "amount")).value_or(0);
        const std::string fee = text(tx, "fee");
        b += "<tr><td>" + number(static_cast<std::int64_t>(shown)) + "</td><td>" + tx_link(text(tx, "txid")) + "</td><td class=\"num\">" +
             (flag(tx, "coinbase") ? std::string("coinbase") : number(static_cast<std::int64_t>(list(tx, "vin").size()))) +
             "</td><td class=\"num\">" + number(static_cast<std::int64_t>(list(tx, "vout").size())) + "</td><td class=\"num\">" +
             coins(format_coins(total)) + "</td><td class=\"num\">" + (fee.empty() ? std::string("<span class=\"muted\">-</span>") : coins(fee)) +
             "</td></tr>";
        ++shown;
    }
    b += "</table></div>";
    if (txs.size() > shown) b += "<p class=\"muted\">And " + number(static_cast<std::int64_t>(txs.size() - shown)) + " more.</p>";
    return {200, "Block " + number(height), std::move(b), {}};
}

Explorer::Page Explorer::transaction(std::string_view id) {
    if (!is_hash(id)) return message(404, "Not found", "That isn't a transaction id.");
    const std::string txid(id);
    const json tx = call("getrawtransaction", json::array({txid, 2}));
    const std::int64_t now = clock_();
    const std::int64_t confirmations = integer(tx, "confirmations", -1);
    const std::string block_hash = text(tx, "blockhash");

    std::string status;
    if (is_hash(block_hash)) {
        status = "In block " + link("/block/" + block_hash, number(integer(tx, "blockheight"))) + ", " + number(confirmations) +
                 (confirmations == 1 ? " confirmation" : " confirmations") + ", " + when(integer(tx, "blocktime"), now);
    } else if (confirmations == 0) {
        status = "Waiting in the " + link("/mempool", "mempool") + " since " + when(integer(tx, "time"), now);
    } else {
        status = "<span class=\"bad\">Not in the chain or the mempool</span>";
    }
    std::string b = "<h1>Transaction</h1><div class=\"wide\"><table class=\"kv\">";
    b += row("Id", mono(esc(txid)));
    b += row("Status", status);
    b += row("Size", size_text(integer(tx, "size")));
    if (const std::string fee = text(tx, "fee"); !fee.empty()) {
        b += row("Fee", coins(fee) + std::format(" <span class=\"muted\">({:.2f} units per byte)</span>", real(tx, "fee_rate")));
    }
    if (const std::string memo = text(tx, "memo"); !memo.empty()) {
        const std::string readable = text(tx, "memo_text");
        b += row("Memo", (readable.empty() ? std::string() : esc(readable) + " ") + "<span class=\"mono muted\">" + esc(memo) + "</span>");
    }
    b += "</table></div><h2>Inputs</h2><div class=\"wide\"><table>";
    if (flag(tx, "coinbase")) {
        b += "<tr><td>Coinbase: the block's new coins and its transactions' fees.</td></tr>";
    } else {
        b += "<tr><th>#</th><th>Spends</th><th>From</th><th class=\"num\">Amount</th></tr>";
        std::int64_t n = 0;
        for (const json& in : list(tx, "vin")) {
            const std::string from = text(in, "txid");
            const json prevout = in.is_object() && in.contains("prevout") ? in["prevout"] : json(nullptr);
            b += "<tr><td>" + number(n++) + "</td><td>" + mono(link("/tx/" + from, esc(from)) + ":" + number(integer(in, "vout"))) + "</td><td>" +
                 (prevout.is_object() ? address_link(text(prevout, "address")) : std::string("<span class=\"muted\">unknown</span>")) +
                 "</td><td class=\"num\">" + (prevout.is_object() ? coins(text(prevout, "amount")) : std::string()) + "</td></tr>";
        }
    }
    b += "</table></div><h2>Outputs</h2><div class=\"wide\"><table><tr><th>#</th><th>To</th><th class=\"num\">Amount</th><th>State</th></tr>";
    for (const json& out : list(tx, "vout")) {
        std::string state;
        if (out.is_object() && out.contains("spendable")) {
            state = "<span class=\"muted\">Can never be spent</span>";
        } else if (flag(out, "spent")) {
            const std::string by = text(out, "spent_by");
            state = "Spent" + (is_hash(by) ? " by " + mono(link("/tx/" + by, esc(by.substr(0, 16)) + "...")) : std::string());
        } else if (const std::string by = text(out, "spent_by_unconfirmed"); is_hash(by)) {
            state = "Being spent by " + mono(link("/tx/" + by, esc(by.substr(0, 16)) + "...")) + " (waiting)";
        } else {
            state = "<span class=\"good\">Unspent</span>";
        }
        b += "<tr><td>" + number(integer(out, "n")) + "</td><td>" + address_link(text(out, "address")) + "</td><td class=\"num\">" +
             coins(text(out, "amount")) + "</td><td>" + state + "</td></tr>";
    }
    b += "</table></div><details><summary>Raw transaction</summary><p class=\"mono\">" + esc(text(tx, "hex")) + "</p></details>";
    return {200, "Transaction " + txid.substr(0, 12), std::move(b), {}};
}

Explorer::Page Explorer::address(std::string_view id, std::size_t page) {
    if (!is_address_like(id)) return message(404, "Not found", "That isn't a BOUND address.");
    const std::string address(id);
    if (!flag(call("validateaddress", json::array({address})), "isvalid")) {
        return message(404, "Not found", "That isn't a BOUND " + options_.network + " address.");
    }
    json summary;
    try {
        summary = call("getaddressbalance", json::array({address}));
    } catch (const RpcError& e) {
        if (e.code != rpc_error::kMisc) throw;
        return {200, "Address",
                "<h1>Address</h1><p class=\"mono\">" + esc(address) +
                    "</p><p>This explorer's node keeps no address index, so balances and history aren't available here. "
                    "Its operator can turn the index on by starting the node with <code>--index</code>.</p>",
                {}};
    }
    const std::int64_t now = clock_();
    const std::size_t count = static_cast<std::size_t>(std::max<std::int64_t>(0, integer(summary, "txcount")));
    const std::size_t pages = std::max<std::size_t>(1, (count + options_.page_size - 1) / options_.page_size);
    page = std::clamp<std::size_t>(page, 1, pages);
    const json deltas = call("getaddressdeltas", json::array({address, options_.page_size, (page - 1) * options_.page_size}));

    std::string b = "<h1>Address</h1><p class=\"mono\">" + esc(address) + "</p><div class=\"cards\">";
    b += card("Balance", coins(text(summary, "balance")));
    b += card("Received", coins(text(summary, "received")));
    b += card("Sent", coins(text(summary, "sent")));
    b += card("Transactions", number(static_cast<std::int64_t>(count)));
    b += card("Unspent outputs", number(integer(summary, "utxos")));
    b += "</div><h2>Transactions</h2><div class=\"wide\"><table><tr><th>When</th><th>Block</th><th>Transaction</th>"
         "<th class=\"num\">Received</th><th class=\"num\">Spent</th><th class=\"num\">Net</th></tr>";
    for (const json& d : deltas.is_array() ? deltas : json::array()) {
        const std::string net = text(d, "delta");
        const std::int64_t height = integer(d, "height");
        b += "<tr><td>" + when(integer(d, "time"), now) + "</td><td>" + link("/block/" + std::to_string(height), number(height)) + "</td><td>" +
             tx_link(text(d, "txid")) + "</td><td class=\"num\">" + coins(text(d, "received")) + "</td><td class=\"num\">" +
             coins(text(d, "spent")) + "</td><td class=\"num " + (net.starts_with("-") ? "bad" : "good") + "\">" + coins(net) + "</td></tr>";
    }
    b += "</table></div><div class=\"pager\">";
    if (page > 1) b += link("/address/" + address + "?page=" + std::to_string(page - 1), "&larr; Newer");
    b += "<span class=\"muted\">Page " + number(static_cast<std::int64_t>(page)) + " of " + number(static_cast<std::int64_t>(pages)) + "</span>";
    if (page < pages) b += link("/address/" + address + "?page=" + std::to_string(page + 1), "Older &rarr;");
    b += "</div><p class=\"muted\">Confirmed transactions only; newest first.</p>";
    return {200, "Address " + address.substr(0, 12), std::move(b), {}};
}

Explorer::Page Explorer::mempool() {
    const std::int64_t now = clock_();
    const json info = call("getmempoolinfo");
    const json entries = call("getrawmempool", json::array({true}));

    struct Row {
        std::string txid;
        std::int64_t time = 0;
        std::int64_t size = 0;
        std::string fee;
        double rate = 0;
    };
    std::vector<Row> rows;
    if (entries.is_object()) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            rows.push_back({it.key(), integer(it.value(), "time"), integer(it.value(), "size"), text(it.value(), "fee"), real(it.value(), "fee_rate")});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.time != b.time ? a.time > b.time : a.txid < b.txid; });

    std::string b = "<h1>Mempool</h1><p class=\"muted\">Transactions this node has checked and is holding for the next blocks.</p><div class=\"cards\">";
    b += card("Transactions", number(integer(info, "size")));
    b += card("Total size", size_text(integer(info, "bytes")));
    b += card("Total fees", coins(text(info, "total_fee")));
    b += card("Lowest fee rate relayed", std::format("{:.2f} units/byte", real(info, "min_relay_fee_rate")));
    b += card("To make the next block", std::format("{:.2f} units/byte", real(info, "next_block_fee_rate")));
    b += "</div><div class=\"wide\"><table><tr><th>Transaction</th><th>Waiting since</th><th class=\"num\">Size</th>"
         "<th class=\"num\">Fee</th><th class=\"num\">Fee rate</th></tr>";
    std::size_t shown = 0;
    for (const Row& r : rows) {
        if (shown++ == options_.max_rows) break;
        b += "<tr><td>" + tx_link(r.txid) + "</td><td>" + when(r.time, now) + "</td><td class=\"num\">" + size_text(r.size) +
             "</td><td class=\"num\">" + coins(r.fee) + "</td><td class=\"num\">" + std::format("{:.2f}", r.rate) + "</td></tr>";
    }
    b += "</table></div>";
    if (rows.empty()) b += "<p>Nothing is waiting.</p>";
    if (rows.size() > options_.max_rows) b += "<p class=\"muted\">And " + number(static_cast<std::int64_t>(rows.size() - options_.max_rows)) + " more.</p>";
    return {200, "Mempool", std::move(b), {}};
}

Explorer::Page Explorer::search(std::string_view input) {
    const std::string q = trim(input);
    const auto go = [](std::string where) { return Page{302, {}, {}, std::move(where)}; };
    if (q.empty()) return message(200, "Search", "Type a block height, a block hash, a transaction id or an address.");
    if (is_height(q)) return go("/block/" + q);
    if (is_hash(q)) {
        try {
            call("getblockheader", json::array({q}));
            return go("/block/" + q);
        } catch (const RpcError&) {
            // not a block: maybe a transaction
        }
        try {
            call("getrawtransaction", json::array({q, 0}));
            return go("/tx/" + q);
        } catch (const RpcError&) {
            // nor one the node can find
        }
        return message(404, "Nothing found", "No block or transaction the node can look up has that id.");
    }
    if (is_address_like(q) && flag(call("validateaddress", json::array({q})), "isvalid")) return go("/address/" + q);
    return message(404, "Nothing found", "\"" + q + "\" isn't a block height, a block hash, a transaction id or an address on this network.");
}

}  // namespace bound
