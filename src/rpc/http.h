#pragma once

#include "net/socket.h"
#include "util/bytes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bound {

// Milestone 7: just enough HTTP/1.1 for JSON-RPC (Bitcoin Core uses libevent's server for this):
// requests with a Content-Length body, keep-alive, no chunked encoding.

inline constexpr std::size_t kMaxHttpHeader = 16 << 10;
inline constexpr std::size_t kMaxHttpBody = 8 << 20;  // room for sendrawtransaction of any transaction

struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;  // names lowercased, values trimmed
    std::string body;
    bool keep_alive = true;  // HTTP/1.1 unless "Connection: close"; HTTP/1.0 only with "keep-alive"
    NetAddress peer;         // who sent it (set by HttpServer)

    std::optional<std::string> header(const std::string& lowercase_name) const;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;  // any others
};

// A request that can't be served: answered with `status` (400, 413, 431, 501), then the
// connection closes.
struct HttpError : std::runtime_error {
    HttpError(int s, const std::string& what) : std::runtime_error(what), status(s) {}
    int status;
};

// Cuts one connection's bytes into requests.
class HttpRequestReader {
public:
    void feed(ByteSpan data) { buf_.append(reinterpret_cast<const char*>(data.data()), data.size()); }
    // The next complete request, if it has all arrived. Throws HttpError if it's malformed or too big.
    std::optional<HttpRequest> next();
    std::size_t buffered() const { return buf_.size(); }  // bytes of a request still arriving

private:
    std::string buf_;
};

const char* http_reason(int status);
std::string format_http_response(const HttpResponse& response, bool keep_alive);
// A complete response at the start of `data`; nullopt if more is needed. `closed`: no more is
// coming, so a response without Content-Length ends here. Throws std::runtime_error if malformed.
std::optional<HttpResponse> parse_http_response(std::string_view data, bool closed);

struct HttpServerOptions {
    std::uint16_t port = 0;            // 0: the OS picks one
    bool loopback_only = true;         // 127.0.0.1 only, as the RPC server must be; false: every interface
    std::size_t max_connections = 16;  // more are turned away
    // Milestone 8, for a server strangers reach (the explorer):
    std::size_t max_per_ip = 0;        // connections one address may hold (not this machine's own); 0: no limit
    std::int64_t idle_timeout = 60;    // seconds without a byte before a connection is dropped
    std::int64_t request_timeout = 0;  // seconds a request may take to arrive in full; 0: no limit
};

// Serves HTTP as a PollClient in someone else's poll loop, so the handler runs on that loop's
// thread. By default on 127.0.0.1 only (Bitcoin Core's RPC default).
class HttpServer final : public PollClient {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    // Throws std::runtime_error if the port is taken.
    HttpServer(const HttpServerOptions& options, Handler handler);
    // On 127.0.0.1:port.
    HttpServer(std::uint16_t port, Handler handler, std::size_t max_connections = 16, std::int64_t idle_timeout = 60);

    std::uint16_t port() const { return listener_.local_port(); }
    std::size_t connections() const { return connections_.size(); }

    void add_poll_items(std::vector<PollItem>& items) override;
    void handle_poll(std::span<const PollItem> items, std::int64_t now) override;

private:
    struct Connection {
        Socket socket;
        NetAddress peer;
        HttpRequestReader reader;
        std::string out;
        std::size_t out_pos = 0;
        bool closing = false;  // close once `out` is sent
        std::int64_t last_active = 0;
        std::int64_t request_started = 0;  // when the request now arriving began; 0: none is
    };

    bool read(Connection& c, std::int64_t now);
    bool write(Connection& c);

    HttpServerOptions options_;
    Socket listener_;
    Handler handler_;
    std::map<std::uint64_t, Connection> connections_;
    std::vector<std::uint64_t> polled_;  // the connections add_poll_items listed, in that order
    std::uint64_t next_id_ = 1;
};

// The client side, for bound-cli and tests: one POST on a new connection, the whole exchange
// within timeout_ms. Throws HttpConnectError if nothing answers at `to`, std::runtime_error if the
// reply is malformed or late.
struct HttpConnectError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
HttpResponse http_post(const NetAddress& to, std::string_view target, std::string_view body,
                       const std::vector<std::pair<std::string, std::string>>& headers, int timeout_ms);

}  // namespace bound
