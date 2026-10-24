#pragma once

#include "httpaceproxycpp/ace_client.hpp"
#include "httpaceproxycpp/config.hpp"
#include "httpaceproxycpp/http_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace httpace {

enum class PushResult {
    Ok,
    DroppedOldest,
    Closed,
};

// One immutable chunk buffer, shared by every client queue that receives it, so the
// fan-out never copies the payload once per client (S2, S5).
using ChunkPtr = std::shared_ptr<const std::vector<char>>;

class ChunkQueue {
public:
    explicit ChunkQueue(std::size_t max_chunks);
    // Never blocks: when the queue is full the oldest chunk is dropped. That is what
    // lets the single shared reader thread ignore one stalled client (S2).
    PushResult push(ChunkPtr chunk);
    bool pop(ChunkPtr& chunk);
    void close();
    std::size_t size() const;

private:
    std::size_t max_chunks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_data_;
    std::deque<ChunkPtr> chunks_;
    bool closed_ = false;
};

struct StreamClient {
    std::string session_id;
    std::string client_ip;
    std::string user_agent;
    std::string channel_name;
    std::string channel_icon;
    std::int64_t connection_time = 0;
    std::shared_ptr<ChunkQueue> queue;
    std::weak_ptr<AceClient> ace;
    std::atomic<std::int64_t> last_activity{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<int> dropped_chunks{0};
    std::atomic<bool> stuck_logged{false};
};

class Broadcast : public std::enable_shared_from_this<Broadcast> {
public:
    Broadcast(std::string infohash, Config config, HttpClient& http_client,
              std::map<std::string, std::string> start_params);
    ~Broadcast();

    std::shared_ptr<StreamClient> add_client(const std::string& client_ip,
                                             const std::string& user_agent,
                                             const std::string& channel_name,
                                             const std::string& channel_icon);
    void remove_client(const std::shared_ptr<StreamClient>& client);
    std::size_t client_count() const;
    std::vector<std::shared_ptr<StreamClient>> clients() const;
    void start_once();
    void stop();
    std::shared_ptr<AceClient> ace() const { return ace_; }
    std::string infohash() const { return infohash_; }

private:
    void stream_loop();
    void stream_http_url(const std::string& url);
    void stream_hls_url(const std::string& url);
    void broadcast_chunk(const char* data, std::size_t size);

    std::string infohash_;
    Config config_;
    HttpClient& http_client_;
    std::map<std::string, std::string> start_params_;
    std::shared_ptr<AceClient> ace_;
    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<StreamClient>> clients_;
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    std::thread stream_thread_;
};

class BroadcastManager {
public:
    BroadcastManager(Config config, HttpClient& http_client);
    ~BroadcastManager();

    std::shared_ptr<Broadcast> get_or_create(const std::string& infohash,
                                             const std::map<std::string, std::string>& params);
    std::shared_ptr<Broadcast> find(const std::string& infohash) const;
    void remove_if_empty(const std::string& infohash);
    std::size_t broadcast_count() const;
    std::size_t client_count() const;
    std::vector<std::shared_ptr<StreamClient>> all_clients() const;
    void stop_all();

private:
    // One slot per infohash that is currently being constructed, so the blocking
    // AceClient::authenticate() inside the Broadcast constructor runs without holding
    // mutex_ while still yielding exactly one Broadcast per infohash.
    struct PendingCreation {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::shared_ptr<Broadcast> broadcast;
        std::exception_ptr error;
    };

    Config config_;
    HttpClient& http_client_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Broadcast>> broadcasts_;
    std::map<std::string, std::shared_ptr<PendingCreation>> creating_;
    bool shutting_down_ = false;
};

} // namespace httpace
