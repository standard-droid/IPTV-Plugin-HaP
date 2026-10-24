#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace httpace {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string client_ip;

    std::string header(const std::string& name, const std::string& fallback = "") const;
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
    bool close = true;
    bool raw_sent = false;
};

class ClientConnection {
public:
    explicit ClientConnection(int fd);
    ~ClientConnection();

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    int fd() const { return fd_; }
    // True when the peer has closed (or reset) the connection. Never blocks.
    bool peer_closed() const;
    bool send_all(const void* data, std::size_t size);
    bool send_text(const std::string& value);
    bool send_response_headers(int status, const std::string& reason,
                               const std::map<std::string, std::string>& headers);
    void close();

private:
    int fd_ = -1;
};

using HttpHandler = std::function<void(const HttpRequest&, ClientConnection&)>;

class HttpServer {
public:
    HttpServer(std::string host, int port, HttpHandler handler);
    ~HttpServer();

    void start();
    void stop();
    void join();
    void set_client_send_timeout(int seconds) { client_send_timeout_ = seconds; }
    void set_client_recv_timeout(int seconds) { client_recv_timeout_ = seconds; }
    void set_max_connections(int max_connections) { max_connections_ = max_connections; }
    std::size_t connection_count() const;
    // Waits (bounded) until every accepted connection has finished its handler.
    bool wait_for_connections(std::chrono::milliseconds timeout);

private:
    // Shared with the detached connection threads so they never touch this object:
    // it keeps the accounting valid even while the server itself is being destroyed.
    struct ConnectionTracker {
        std::atomic<int> active{0};
        std::mutex mutex;
        std::condition_variable cv;
        // Live client sockets, so stop() can unblock a handler stuck in send().
        std::set<int> fds;
    };

    void accept_loop();
    static void serve_client(int client_fd, std::string client_ip, HttpHandler handler,
                             std::shared_ptr<ConnectionTracker> tracker);

    std::string host_;
    int port_;
    HttpHandler handler_;
    std::atomic<int> listen_fd_{-1};
    int client_send_timeout_ = 0;
    int client_recv_timeout_ = 10;
    // Server-side concurrency bound (0 = unlimited). This is its own limit, deliberately
    // separate from Config::max_connections, which caps concurrent *streaming* clients.
    int max_connections_ = 32;
    std::shared_ptr<ConnectionTracker> connections_ = std::make_shared<ConnectionTracker>();
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};

std::string status_reason(int status);
bool parse_http_request(int fd, const std::string& client_ip, HttpRequest& request);
void send_simple_response(ClientConnection& connection, int status, const std::string& content_type,
                          const std::string& body,
                          std::map<std::string, std::string> extra_headers = {});

} // namespace httpace
