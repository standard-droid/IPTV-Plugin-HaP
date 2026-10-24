#include "httpaceproxycpp/http_server.hpp"
#include "httpaceproxycpp/util.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace httpace {
namespace {

constexpr int kAcceptBackoffMinMs = 10;
constexpr int kAcceptBackoffMaxMs = 500;
constexpr int kAcceptBackoffSliceMs = 10;
// Bounded transient retries: a signal must never fail a send, and a full write buffer
// must not let one connection pin its thread forever. Each EAGAIN already waited for the
// whole SO_SNDTIMEO window, so only a couple of those are tolerated.
constexpr int kMaxInterruptedSends = 1000;
constexpr int kMaxWouldBlockSends = 2;
constexpr int kMaxInterruptedReads = 100;

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool send_fd_all(int fd, const void* data, std::size_t size) {
    const char* ptr = static_cast<const char*>(data);
    int interrupted = 0;
    int would_block = 0;
    while (size > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t sent = ::send(fd, ptr, size, MSG_NOSIGNAL);
#else
        ssize_t sent = ::send(fd, ptr, size, 0);
#endif
        if (sent > 0) {
            ptr += sent;
            size -= static_cast<std::size_t>(sent);
            interrupted = 0;
            would_block = 0;
            continue;
        }
        if (sent < 0 && errno == EINTR && ++interrupted <= kMaxInterruptedSends) continue;
        // With SO_SNDTIMEO a full buffer reports EAGAIN once the write timeout expires.
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            ++would_block <= kMaxWouldBlockSends) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        return false;
    }
    return true;
}

std::string read_until_headers(int fd) {
    std::string buffer;
    buffer.reserve(8192);
    char tmp[4096];
    int interrupted = 0;
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0 && errno == EINTR && ++interrupted <= kMaxInterruptedReads) continue;
        if (n <= 0) return "";
        buffer.append(tmp, static_cast<std::size_t>(n));
        if (buffer.size() > 1024 * 128) return "";
    }
    return buffer;
}

// Sleeps in small slices while the server is running, so stop() is honoured promptly.
void sleep_while_running(const std::atomic<bool>& running, int milliseconds) {
    int slept = 0;
    while (running && slept < milliseconds) {
        int slice = std::min(kAcceptBackoffSliceMs, milliseconds - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept += slice;
    }
}

} // namespace

std::string HttpRequest::header(const std::string& name, const std::string& fallback) const {
    auto it = headers.find(lower(name));
    return it == headers.end() ? fallback : it->second;
}

ClientConnection::ClientConnection(int fd) : fd_(fd) {}
ClientConnection::~ClientConnection() { close(); }

bool ClientConnection::peer_closed() const {
    if (fd_ < 0) return true;
    char probe = 0;
    for (;;) {
        // MSG_DONTWAIT keeps this non-blocking regardless of SO_RCVTIMEO.
        ssize_t n = ::recv(fd_, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) return false;   // pending request bytes: peer is still there
        if (n == 0) return true;   // orderly shutdown by the peer
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;               // ECONNRESET and friends
    }
}

bool ClientConnection::send_all(const void* data, std::size_t size) {
    return fd_ >= 0 && send_fd_all(fd_, data, size);
}

bool ClientConnection::send_text(const std::string& value) {
    return send_all(value.data(), value.size());
}

bool ClientConnection::send_response_headers(int status, const std::string& reason,
                                             const std::map<std::string, std::string>& headers) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << " " << reason << "\r\n";
    for (const auto& [k, v] : headers) out << k << ": " << v << "\r\n";
    out << "\r\n";
    auto data = out.str();
    return send_text(data);
}

void ClientConnection::close() { close_fd(fd_); }

HttpServer::HttpServer(std::string host, int port, HttpHandler handler)
    : host_(std::move(host)), port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
    stop();
    join();
    // Connection threads are detached, so make sure none of them is still running (and
    // still holding handler_, which points at the owning Proxy) before this object dies.
    if (!wait_for_connections(std::chrono::milliseconds(5000))) {
        log_line("WARNING", "HTTP server destroyed with client connections still active");
    }
}

void HttpServer::start() {
    if (running_) return;
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (host_.empty() || host_ == "0.0.0.0") addr.sin_addr.s_addr = INADDR_ANY;
    else if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        close_fd(fd);
        throw std::runtime_error("invalid listen host: " + host_);
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        auto err = std::string(std::strerror(errno));
        close_fd(fd);
        throw std::runtime_error("bind failed: " + err);
    }
    if (::listen(fd, 128) < 0) {
        auto err = std::string(std::strerror(errno));
        close_fd(fd);
        throw std::runtime_error("listen failed: " + err);
    }
    listen_fd_.store(fd);
    running_ = true;
    accept_thread_ = std::thread(&HttpServer::accept_loop, this);
}

void HttpServer::stop() {
    running_ = false;
    // Only shutdown() here: close() is deferred to join() so a listening fd is never
    // recycled (and thus reused by another thread's socket) while accept() may still
    // be running on it.
    int fd = listen_fd_.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    // Unblock handlers that are stuck writing to a stalled client, so shutdown does not
    // have to wait for their socket timeout.
    auto tracker = connections_;
    std::lock_guard<std::mutex> lock(tracker->mutex);
    for (int client_fd : tracker->fds) ::shutdown(client_fd, SHUT_RDWR);
}

void HttpServer::join() {
    if (accept_thread_.joinable()) accept_thread_.join();
    int fd = listen_fd_.exchange(-1);
    if (fd >= 0) ::close(fd);
}

std::size_t HttpServer::connection_count() const {
    int active = connections_->active.load(std::memory_order_acquire);
    return active > 0 ? static_cast<std::size_t>(active) : 0;
}

bool HttpServer::wait_for_connections(std::chrono::milliseconds timeout) {
    auto tracker = connections_;
    std::unique_lock<std::mutex> lock(tracker->mutex);
    return tracker->cv.wait_for(lock, timeout, [&] {
        return tracker->active.load(std::memory_order_acquire) <= 0;
    });
}

void HttpServer::accept_loop() {
    int backoff_ms = 0;
    while (running_) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int listen_fd = listen_fd_.load();
        int fd = listen_fd >= 0 ? ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client), &len) : -1;
        if (fd < 0) {
            if (!running_) break;
            int err = listen_fd < 0 ? EBADF : errno;
            if (err == EINTR || err == ECONNABORTED) continue;
            if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM ||
                err == EAGAIN || err == EWOULDBLOCK) {
                // Resource exhaustion: back off exponentially instead of spinning at 100% CPU.
                backoff_ms = backoff_ms == 0
                    ? kAcceptBackoffMinMs
                    : std::min(backoff_ms * 2, kAcceptBackoffMaxMs);
                log_line("ERROR", "accept failed: " + std::string(std::strerror(err)) +
                         " (retrying in " + std::to_string(backoff_ms) + " ms)");
                sleep_while_running(running_, backoff_ms);
                continue;
            }
            if (err == EBADF || err == EINVAL || err == ENOTSOCK || err == EFAULT) {
                log_line("ERROR", "accept failed fatally: " + std::string(std::strerror(err)) +
                         " (stopping accept loop)");
                break;
            }
            log_line("WARNING", "accept failed: " + std::string(std::strerror(err)));
            sleep_while_running(running_, kAcceptBackoffMinMs);
            continue;
        }
        backoff_ms = 0;

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        if (client_send_timeout_ > 0) {
            timeval tv{client_send_timeout_, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (client_recv_timeout_ > 0) {
            // Without this, a client that connects and never sends a request holds its
            // worker thread forever.
            timeval tv{client_recv_timeout_, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        auto tracker = connections_;
        bool over_capacity = false;
        {
            std::lock_guard<std::mutex> lock(tracker->mutex);
            if (max_connections_ > 0 && tracker->active.load(std::memory_order_acquire) >= max_connections_) {
                over_capacity = true;
            } else {
                tracker->active.fetch_add(1, std::memory_order_acq_rel);
                tracker->fds.insert(fd);
            }
        }
        if (over_capacity) {
            ClientConnection rejected_connection(fd);
            send_simple_response(rejected_connection, 503, "text/plain", "Too many concurrent connections");
            continue;
        }

        try {
            std::thread([tracker, handler = handler_, fd, client_ip = std::string(ip)]() mutable {
                serve_client(fd, std::move(client_ip), std::move(handler), std::move(tracker));
            }).detach();
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(tracker->mutex);
                tracker->fds.erase(fd);
                tracker->active.fetch_sub(1, std::memory_order_acq_rel);
            }
            tracker->cv.notify_all();
            close_fd(fd);
            log_line("ERROR", "cannot spawn client thread: " + std::string(e.what()));
            sleep_while_running(running_, kAcceptBackoffMinMs);
        }
    }
}

void HttpServer::serve_client(int client_fd, std::string client_ip, HttpHandler handler,
                              std::shared_ptr<ConnectionTracker> tracker) {
    ClientConnection connection(client_fd);
    struct ActiveGuard {
        std::shared_ptr<ConnectionTracker> tracker;
        ClientConnection* connection;
        ~ActiveGuard() {
            {
                // Unregister and close under the mutex stop() takes, so stop() can never
                // shutdown() a descriptor that was already closed and recycled.
                std::lock_guard<std::mutex> lock(tracker->mutex);
                tracker->fds.erase(connection->fd());
                connection->close();
                tracker->active.fetch_sub(1, std::memory_order_acq_rel);
            }
            tracker->cv.notify_all();
        }
    } guard{tracker, &connection};
    try {
        HttpRequest request;
        if (!parse_http_request(client_fd, client_ip, request)) return;
        handler(request, connection);
    } catch (const std::exception& e) {
        send_simple_response(connection, 500, "text/plain", e.what());
    }
}

std::string status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

bool parse_http_request(int fd, const std::string& client_ip, HttpRequest& request) {
    auto raw = read_until_headers(fd);
    if (raw.empty()) return false;
    auto header_end = raw.find("\r\n\r\n");
    auto head = raw.substr(0, header_end);
    std::istringstream in(head);
    std::string request_line;
    std::getline(in, request_line);
    request_line = trim(request_line);
    auto parts = split(request_line, ' ', false);
    if (parts.size() < 3) return false;
    request.method = parts[0];
    request.target = parts[1];
    request.version = parts[2];
    request.client_ip = client_ip;
    auto q = request.target.find('?');
    request.path = q == std::string::npos ? request.target : request.target.substr(0, q);
    request.query = q == std::string::npos ? "" : request.target.substr(q + 1);

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return true;
}

void send_simple_response(ClientConnection& connection, int status, const std::string& content_type,
                          const std::string& body,
                          std::map<std::string, std::string> extra_headers) {
    extra_headers["Content-Type"] = content_type;
    extra_headers["Content-Length"] = std::to_string(body.size());
    extra_headers["Connection"] = "close";
    connection.send_response_headers(status, status_reason(status), extra_headers);
    if (!body.empty()) connection.send_text(body);
}

} // namespace httpace
