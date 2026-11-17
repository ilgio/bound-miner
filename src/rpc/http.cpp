#include "rpc/http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <exception>
#include <format>

namespace bound {

namespace {

std::string lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Splits the head (everything before the blank line) into its first line and its header fields.
// Throws std::invalid_argument on a field without a colon.
std::string_view split_head(std::string_view head, std::map<std::string, std::string>& headers) {
    std::size_t eol = head.find("\r\n");
    const std::string_view first = head.substr(0, eol);
    while (eol != std::string_view::npos) {
        const std::size_t start = eol + 2;
        eol = head.find("\r\n", start);
        const std::string_view line = head.substr(start, eol == std::string_view::npos ? std::string_view::npos : eol - start);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) throw std::invalid_argument("malformed header field");
        headers[lowercase(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
    }
    return first;
}

// nullopt unless all digits, and at most `max`.
std::optional<std::size_t> parse_length(std::string_view text, std::size_t max) {
    if (text.empty() || text.size() > 12) return std::nullopt;
    std::size_t n = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    if (n > max) return std::nullopt;
    return n;
}

}  // namespace

std::optional<std::string> HttpRequest::header(const std::string& lowercase_name) const {
    const auto found = headers.find(lowercase_name);
    if (found == headers.end()) return std::nullopt;
    return found->second;
}

std::optional<HttpRequest> HttpRequestReader::next() {
    const std::size_t head_end = buf_.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        if (buf_.size() > kMaxHttpHeader) throw HttpError(431, "request header too large");
        return std::nullopt;
    }
    if (head_end > kMaxHttpHeader) throw HttpError(431, "request header too large");

    HttpRequest request;
    std::string_view first;
    try {
        first = split_head(std::string_view(buf_).substr(0, head_end), request.headers);
    } catch (const std::invalid_argument&) {
        throw HttpError(400, "malformed header field");
    }
    const std::size_t sp1 = first.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : first.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || first.find(' ', sp2 + 1) != std::string_view::npos) throw HttpError(400, "malformed request line");
    request.method = std::string(first.substr(0, sp1));
    request.target = std::string(first.substr(sp1 + 1, sp2 - sp1 - 1));
    const std::string version(first.substr(sp2 + 1));  // a copy: buf_ is cut below
    if (version != "HTTP/1.1" && version != "HTTP/1.0") throw HttpError(400, "unsupported HTTP version");
    if (request.method.empty() || request.target.empty()) throw HttpError(400, "malformed request line");

    if (request.headers.contains("transfer-encoding")) throw HttpError(501, "chunked bodies aren't supported");
    std::size_t length = 0;
    if (const auto value = request.header("content-length")) {
        const std::optional<std::size_t> parsed = parse_length(*value, kMaxHttpBody);
        if (!parsed) throw HttpError(413, "bad or too large Content-Length");
        length = *parsed;
    }
    const std::size_t body_start = head_end + 4;
    if (buf_.size() - body_start < length) return std::nullopt;

    request.body = buf_.substr(body_start, length);
    buf_.erase(0, body_start + length);
    const std::string connection = lowercase(request.header("connection").value_or(""));
    request.keep_alive = version == "HTTP/1.1" ? connection != "close" : connection == "keep-alive";
    return request;
}

const char* http_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    }
    return "Unknown";
}

std::string format_http_response(const HttpResponse& response, bool keep_alive) {
    std::string out = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n", response.status,
                                  http_reason(response.status), response.content_type, response.body.size());
    for (const auto& [name, value] : response.headers) out += name + ": " + value + "\r\n";
    if (!keep_alive) out += "Connection: close\r\n";
    out += "\r\n";
    out += response.body;
    return out;
}

std::optional<HttpResponse> parse_http_response(std::string_view data, bool closed) {
    const std::size_t head_end = data.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        if (closed) throw std::runtime_error("the connection closed in the middle of an HTTP reply");
        return std::nullopt;
    }
    std::map<std::string, std::string> headers;
    std::string_view first;
    try {
        first = split_head(data.substr(0, head_end), headers);
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("malformed HTTP reply");
    }
    // "HTTP/1.1 200 OK"
    if (first.size() < 12 || !first.starts_with("HTTP/1.") || first[8] != ' ') throw std::runtime_error("malformed HTTP reply");
    const std::optional<std::size_t> status = parse_length(first.substr(9, 3), 999);
    if (!status) throw std::runtime_error("malformed HTTP status");

    HttpResponse response;
    response.status = static_cast<int>(*status);
    if (const auto type = headers.find("content-type"); type != headers.end()) response.content_type = type->second;
    const std::string_view rest = data.substr(head_end + 4);
    if (const auto length = headers.find("content-length"); length != headers.end()) {
        const std::optional<std::size_t> n = parse_length(length->second, kMaxHttpBody * 4);
        if (!n) throw std::runtime_error("bad Content-Length in the HTTP reply");
        if (rest.size() < *n) {
            if (closed) throw std::runtime_error("the connection closed in the middle of an HTTP reply");
            return std::nullopt;
        }
        response.body = std::string(rest.substr(0, *n));
        return response;
    }
    if (!closed) return std::nullopt;
    response.body = std::string(rest);
    return response;
}

HttpServer::HttpServer(const HttpServerOptions& options, Handler handler)
    : options_(options), listener_(Socket::listen(options.port, options.loopback_only)), handler_(std::move(handler)) {}

HttpServer::HttpServer(std::uint16_t port, Handler handler, std::size_t max_connections, std::int64_t idle_timeout)
    : HttpServer(HttpServerOptions{port, true, max_connections, 0, idle_timeout, 0}, std::move(handler)) {}

void HttpServer::add_poll_items(std::vector<PollItem>& items) {
    items.push_back({listener_.handle(), true, false});
    polled_.clear();
    for (const auto& [id, c] : connections_) {
        items.push_back({c.socket.handle(), !c.closing, c.out_pos < c.out.size()});
        polled_.push_back(id);
    }
}

void HttpServer::handle_poll(std::span<const PollItem> items, std::int64_t now) {
    if (items.empty()) return;
    std::vector<std::uint64_t> drop;
    for (std::size_t i = 0; i < polled_.size() && i + 1 < items.size(); ++i) {
        const PollItem& item = items[i + 1];
        const auto found = connections_.find(polled_[i]);
        if (found == connections_.end()) continue;
        if ((item.readable || item.failed) && !read(found->second, now)) drop.push_back(polled_[i]);
    }
    if (items[0].readable) {
        while (std::optional<std::pair<Socket, NetAddress>> accepted = listener_.accept()) {
            if (connections_.size() >= options_.max_connections) continue;  // closes it
            if (options_.max_per_ip > 0 && !accepted->second.is_loopback()) {
                const auto same = static_cast<std::size_t>(std::count_if(connections_.begin(), connections_.end(), [&](const auto& c) {
                    return c.second.peer.ip == accepted->second.ip;
                }));
                if (same >= options_.max_per_ip) continue;
            }
            Connection& c = connections_[next_id_++];
            c.socket = std::move(accepted->first);
            c.peer = accepted->second;
            c.last_active = now;
        }
    }
    for (auto& [id, c] : connections_) {
        if (std::find(drop.begin(), drop.end(), id) != drop.end()) continue;
        const bool slow = options_.request_timeout > 0 && c.request_started != 0 && now - c.request_started > options_.request_timeout;
        if (!write(c)) {
            drop.push_back(id);  // broken
        } else if (c.out.empty() && (c.closing || slow || now - c.last_active > options_.idle_timeout)) {
            drop.push_back(id);  // done, idle, or taking too long to say what it wants
        }
    }
    for (const std::uint64_t id : drop) connections_.erase(id);
}

// false: drop the connection now.
bool HttpServer::read(Connection& c, std::int64_t now) {
    std::array<std::uint8_t, 16 << 10> buffer;
    bool peer_closed = false;
    for (std::size_t total = 0; total < (1 << 20);) {
        const IoResult r = c.socket.recv(buffer);
        if (r.would_block) break;
        if (r.closed) {
            peer_closed = true;  // perhaps only its sending half: still answer what it asked
            break;
        }
        c.reader.feed(ByteSpan(buffer.data(), r.bytes));
        c.last_active = now;
        total += r.bytes;
    }
    while (!c.closing) {
        try {
            std::optional<HttpRequest> request = c.reader.next();
            if (!request) break;
            request->peer = c.peer;
            HttpResponse response;
            try {
                response = handler_(*request);
            } catch (const std::exception& e) {
                response = {500, std::string(e.what()) + "\n", "text/plain", {}};
            }
            std::string text = format_http_response(response, request->keep_alive);
            if (request->method == "HEAD") text.resize(text.size() - response.body.size());  // the headers alone
            c.out += text;
            c.closing = !request->keep_alive;
            c.request_started = 0;
        } catch (const HttpError& e) {
            c.out += format_http_response({e.status, std::string(e.what()) + "\n", "text/plain", {}}, false);
            c.closing = true;
        }
    }
    // A request that's only partly here is on the clock (request_timeout) from its first byte.
    if (c.reader.buffered() == 0) {
        c.request_started = 0;
    } else if (c.request_started == 0) {
        c.request_started = now;
    }
    if (peer_closed) c.closing = true;
    return !(peer_closed && c.out_pos == c.out.size());
}

// false: the connection broke.
bool HttpServer::write(Connection& c) {
    while (c.out_pos < c.out.size()) {
        const std::string_view rest = std::string_view(c.out).substr(c.out_pos);
        const IoResult r = c.socket.send(bytes_of(rest));
        if (r.closed) return false;
        if (r.would_block || r.bytes == 0) return true;
        c.out_pos += r.bytes;
    }
    c.out.clear();
    c.out_pos = 0;
    return true;
}

HttpResponse http_post(const NetAddress& to, std::string_view target, std::string_view body,
                       const std::vector<std::pair<std::string, std::string>>& headers, int timeout_ms) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto remaining = [&] {
        return static_cast<int>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
    };

    Socket socket;
    try {
        socket = Socket::connect(to);
    } catch (const std::runtime_error& e) {
        throw HttpConnectError(e.what());
    }
    while (true) {
        std::array<PollItem, 1> item{PollItem{socket.handle(), false, true}};
        poll(item, std::min(remaining(), 100));
        if (item[0].writable || item[0].failed) {
            if (item[0].failed || socket.connect_error() != 0) throw HttpConnectError("can't connect to " + to.to_string());
            break;
        }
        if (remaining() == 0) throw HttpConnectError("can't connect to " + to.to_string() + ": timed out");
    }

    std::string request = std::format("POST {} HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n"
                                      "Connection: close\r\n",
                                      target, to.to_string(), body.size());
    for (const auto& [name, value] : headers) request += name + ": " + value + "\r\n";
    request += "\r\n";
    request += body;

    std::size_t sent = 0;
    std::string reply;
    std::array<std::uint8_t, 16 << 10> buffer;
    while (true) {
        if (remaining() == 0) throw std::runtime_error("no reply from " + to.to_string() + " in time");
        std::array<PollItem, 1> item{PollItem{socket.handle(), true, sent < request.size()}};
        poll(item, std::min(remaining(), 100));
        if (item[0].writable && sent < request.size()) {
            const IoResult r = socket.send(bytes_of(std::string_view(request).substr(sent)));
            if (r.closed) throw std::runtime_error("the connection to " + to.to_string() + " broke");
            sent += r.bytes;
        }
        if (item[0].readable || item[0].failed) {
            bool closed = false;
            while (true) {
                const IoResult r = socket.recv(buffer);
                if (r.would_block) break;
                if (r.closed) {
                    closed = true;
                    break;
                }
                reply.append(reinterpret_cast<const char*>(buffer.data()), r.bytes);
            }
            if (std::optional<HttpResponse> response = parse_http_response(reply, closed)) return *response;
            if (closed) throw std::runtime_error("the connection closed without a reply");
        }
    }
}

}  // namespace bound
