#include "httpaceproxycpp/broadcast.hpp"
#include "httpaceproxycpp/util.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace httpace {

namespace {

// True if the buffer holds MPEG-TS: three sync bytes (0x47) 188 bytes apart at some
// offset within the first packet. The engine's output does not always start on a
// packet boundary, so checking only the first byte misses real TS streams.
bool looks_like_mpeg_ts(const char* data, std::size_t size) {
    constexpr std::size_t kPacket = 188;
    constexpr std::size_t kNeeded = 3;
    if (size < kPacket * (kNeeded - 1) + 1) return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    for (std::size_t offset = 0; offset < kPacket; ++offset) {
        if (offset + kPacket * (kNeeded - 1) >= size) break;
        bool match = true;
        for (std::size_t i = 0; i < kNeeded && match; ++i) match = bytes[offset + i * kPacket] == 0x47;
        if (match) return true;
    }
    return false;
}

} // namespace

ChunkQueue::ChunkQueue(std::size_t max_chunks) : max_chunks_(std::max<std::size_t>(2, max_chunks)) {}

PushResult ChunkQueue::push(ChunkPtr chunk) {
    if (!chunk) return PushResult::Ok;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return PushResult::Closed;
    PushResult result = PushResult::Ok;
    if (chunks_.size() >= max_chunks_) {
        chunks_.pop_front();
        result = PushResult::DroppedOldest;
    }
    chunks_.push_back(std::move(chunk));
    cv_data_.notify_one();
    return result;
}

bool ChunkQueue::pop(ChunkPtr& chunk) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_data_.wait(lock, [&] { return closed_ || !chunks_.empty(); });
    if (chunks_.empty()) return false;
    chunk = std::move(chunks_.front());
    chunks_.pop_front();
    return true;
}

void ChunkQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_data_.notify_all();
}

std::size_t ChunkQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.size();
}

Broadcast::Broadcast(std::string infohash, Config config, HttpClient& http_client,
                     std::map<std::string, std::string> start_params)
    : infohash_(std::move(infohash)),
      config_(std::move(config)),
      http_client_(http_client),
      start_params_(std::move(start_params)),
      ace_(std::make_shared<AceClient>(config_, "Broadcast_" + infohash_.substr(0, 8))) {
    ace_->authenticate();
}

Broadcast::~Broadcast() { stop(); }

std::shared_ptr<StreamClient> Broadcast::add_client(const std::string& client_ip,
                                                    const std::string& user_agent,
                                                    const std::string& channel_name,
                                                    const std::string& channel_icon) {
    auto client = std::make_shared<StreamClient>();
    client->session_id = std::to_string(reinterpret_cast<std::uintptr_t>(client.get()));
    client->client_ip = client_ip;
    client->user_agent = user_agent;
    client->channel_name = channel_name;
    client->channel_icon = channel_icon.empty() ? "http://static.acestream.net/sites/acestream/img/ACE-logo.png" : channel_icon;
    client->connection_time = unix_time();
    client->last_activity = client->connection_time;
    client->queue = std::make_shared<ChunkQueue>(static_cast<std::size_t>(std::max(2, config_.client_queue_size)));
    client->ace = ace();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.push_back(client);
    }
    // The stream thread may have given up between get_or_create() and here; its final
    // close of every queue would then have missed this one and the client would wait
    // forever for data.
    if (finished()) client->queue->close();
    return client;
}

void Broadcast::remove_client(const std::shared_ptr<StreamClient>& client) {
    if (!client) return;
    client->queue->close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](const auto& weak) {
            auto locked = weak.lock();
            return !locked || locked == client;
        }), clients_.end());
    }
    if (client_count() == 0) stop();
}

std::size_t Broadcast::client_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& weak : clients_) if (!weak.expired()) ++count;
    return count;
}

std::vector<std::shared_ptr<StreamClient>> Broadcast::clients() const {
    std::vector<std::shared_ptr<StreamClient>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& weak : clients_) {
        if (auto client = weak.lock()) out.push_back(client);
    }
    return out;
}

void Broadcast::start_once() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true)) {
        running_ = true;
        stream_thread_ = std::thread(&Broadcast::stream_loop, this);
    }
}

void Broadcast::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) return;
    running_ = false;
    for (auto& client : clients()) client->queue->close();
    if (auto ace = take_ace()) {
        try { ace->stop_broadcast(); } catch (...) {}
        try { ace->shutdown(); } catch (...) {}
    }
    if (stream_thread_.joinable() && stream_thread_.get_id() != std::this_thread::get_id()) stream_thread_.join();
}

std::shared_ptr<AceClient> Broadcast::take_ace() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto old = std::move(ace_);
    ace_.reset();
    return old;
}

// Installs a freshly authenticated engine session, unless stop() has run meanwhile:
// stop() sets stopped_ before taking ace_ under the same mutex, so a session installed
// here is always either seen and shut down by stop() or refused.
bool Broadcast::install_ace(const std::shared_ptr<AceClient>& next) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return false;
    ace_ = next;
    return true;
}

// One engine session: START, then relay the stream until it ends. Returns whether any
// data was relayed.
bool Broadcast::run_attempt() {
    auto ace = this->ace();
    if (!ace) throw std::runtime_error("no engine session");
    any_data_ = false;
    std::atomic<bool> attempt_done{false};
    // While a rebuilt session is starting, keep the players' connections busy so their
    // read timeouts don't fire. send_keepalive() is a no-op until the first session has
    // shown the output is MPEG-TS, so the very first start behaves as before.
    std::thread keepalive([this, &attempt_done] {
        while (!attempt_done && running_ && !any_data_) {
            send_keepalive();
            for (int i = 0; i < 10 && !attempt_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    struct JoinGuard {
        std::atomic<bool>& done;
        std::thread& thread;
        ~JoinGuard() {
            done = true;
            if (thread.joinable()) thread.join();
        }
    } guard{attempt_done, keepalive};
    auto started = ace->start_broadcast(start_params_);
    auto url = rewrite_url_host_port(url_decode(started.url), config_.ace_host, std::to_string(config_.ace_http_port));
    log_line("INFO", "[" + infohash_.substr(0, 8) + "] stream URL " + url);
    if (ends_with(parse_url(url).path, ".m3u8")) stream_hls_url(url);
    else stream_http_url(url);
    return any_data_;
}

// MPEG-TS null packets (PID 0x1FFF): players discard them, but they keep the HTTP
// connection active while the engine session is rebuilt. Only sent while the current
// session has not relayed real data, so they never interleave with stream bytes.
void Broadcast::send_keepalive() {
    static const std::vector<char> null_packets = [] {
        std::vector<char> out;
        for (int i = 0; i < 7; ++i) {
            out.push_back(static_cast<char>(0x47));
            out.push_back(static_cast<char>(0x1F));
            out.push_back(static_cast<char>(0xFF));
            out.push_back(static_cast<char>(0x10));
            out.insert(out.end(), 184, static_cast<char>(0xFF));
        }
        return out;
    }();
    if (ts_output_) broadcast_chunk(null_packets.data(), null_packets.size(), true);
}

void Broadcast::wait_with_keepalive(int seconds) {
    for (int i = 0; i < seconds && running_ && client_count() > 0; ++i) {
        send_keepalive();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Relays the stream and, when the engine side drops while players are still attached,
// rebuilds the engine session instead of closing them (adapted from the pedrosc10 fork).
void Broadcast::stream_loop() {
    const std::string tag = "[" + infohash_.substr(0, 8) + "] ";
    const int max_attempts = std::max(0, config_.reconnect_attempts);
    // Retries continue for at least this long after an outage starts, even past
    // max_attempts: if the AceServe process died, HaP's watchdog needs up to ~15 s to
    // notice and ~20-25 s to start it again, longer than the attempts alone last.
    const auto recovery_window = std::chrono::seconds(std::max(0, config_.reconnect_window));
    int failures = 0;
    auto outage_start = std::chrono::steady_clock::now();
    while (running_ && client_count() > 0) {
        auto attempt_start = std::chrono::steady_clock::now();
        try {
            run_attempt();
        } catch (const std::exception& e) {
            log_line("ERROR", tag + "stream failed: " + std::string(e.what()));
        }
        if (!running_ || client_count() == 0) break;

        auto now = std::chrono::steady_clock::now();
        if (now - attempt_start > std::chrono::seconds(60)) failures = 0;
        if (failures == 0) outage_start = now;
        ++failures;
        if (max_attempts == 0 || (failures > max_attempts && now - outage_start >= recovery_window)) {
            if (max_attempts > 0) {
                log_line("ERROR", tag + "giving up after " + std::to_string(failures - 1) + " reconnect attempts in " +
                         std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - outage_start).count()) + "s");
            }
            break;
        }
        log_line("WARNING", tag + "engine stream dropped, reconnecting (attempt " + std::to_string(failures) + ")");

        if (auto old = take_ace()) {
            try { old->stop_broadcast(); } catch (...) {}
            try { old->shutdown(); } catch (...) {}
        }
        any_data_ = false;
        wait_with_keepalive(std::min(failures * 2, 8));
        if (!running_ || client_count() == 0) break;
        try {
            auto fresh = std::make_shared<AceClient>(config_, "Broadcast_" + infohash_.substr(0, 8));
            fresh->authenticate();
            if (!install_ace(fresh)) {
                try { fresh->shutdown(); } catch (...) {}
                break;
            }
        } catch (const std::exception& e) {
            // The next run_attempt() fails fast with "no engine session" and counts as
            // another failure, so this still ends once the attempts and window run out.
            log_line("ERROR", tag + "engine reconnect failed: " + std::string(e.what()));
        }
    }
    running_ = false;
    for (auto& client : clients()) client->queue->close();
}

void Broadcast::stream_http_url(const std::string& url) {
    http_client_.stream(url, [&](const char* data, std::size_t size) {
        broadcast_chunk(data, size);
        if (!running_ || client_count() == 0) {
            running_ = false;
            return false;
        }
        return true;
    }, running_, 5, config_.video_timeout, std::max(1, config_.curl_stream_buffer));
}

void Broadcast::stream_hls_url(const std::string& url) {
    std::vector<std::string> seen;
    int consecutive_errors = 0;
    while (running_ && client_count() > 0) {
        try {
            auto response = http_client_.get(url, {}, config_.video_timeout);
            auto base = parse_url(url);
            for (const auto& raw_line : split(response.body, '\n', false)) {
                auto line = trim(raw_line);
                if (line.empty() || starts_with(line, "#")) continue;
                std::string segment = line;
                if (!starts_with(segment, "http://") && !starts_with(segment, "https://")) {
                    auto base_path = base.path;
                    auto slash = base_path.find_last_of('/');
                    base_path = slash == std::string::npos ? "/" : base_path.substr(0, slash + 1);
                    segment = base.scheme + "://" + base.authority + base_path + segment;
                }
                if (std::find(seen.begin(), seen.end(), segment) != seen.end()) continue;
                stream_http_url(segment);
                seen.push_back(segment);
                if (seen.size() > 50) seen.erase(seen.begin());
                if (!running_ || client_count() == 0) break;
            }
            consecutive_errors = 0;
        } catch (const std::exception& e) {
            log_line("ERROR", "HLS refresh failed: " + std::string(e.what()));
            // Let stream_loop() rebuild the engine session instead of polling a dead
            // playlist forever.
            if (++consecutive_errors >= 5) return;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void Broadcast::broadcast_chunk(const char* data, std::size_t size, bool keepalive) {
    if (size == 0) return;
    std::lock_guard<std::mutex> chunk_lock(chunk_mutex_);
    if (keepalive) {
        if (any_data_) return;
    } else {
        any_data_ = true;
        // Only probe the start of the stream, so a long non-TS stream cannot match by chance.
        if (!ts_output_ && ts_probe_chunks_ < 64) {
            ++ts_probe_chunks_;
            if (looks_like_mpeg_ts(data, size)) ts_output_ = true;
        }
    }
    // One buffer for every client: the fan-out shares it instead of copying per client.
    auto buffer = std::make_shared<std::vector<char>>(data, data + size);
    ChunkPtr chunk = std::move(buffer);
    auto write_timeout = std::max(1, config_.client_write_timeout);
    auto now = unix_time();
    for (auto& client : clients()) {
        auto result = client->queue->push(chunk);
        if (result == PushResult::Closed) continue;
        if (result == PushResult::DroppedOldest) {
            client->dropped_chunks.fetch_add(1, std::memory_order_relaxed);
            if (now - client->last_activity.load() > write_timeout) {
                if (!client->stuck_logged.exchange(true)) {
                    log_line("WARNING", "[" + client->client_ip + "] client too slow ("
                             + std::to_string(client->dropped_chunks.load()) + " chunks dropped in "
                             + std::to_string(write_timeout) + "s), closing");
                    client->queue->close();
                }
            }
        } else {
            client->dropped_chunks.store(0, std::memory_order_relaxed);
        }
    }
}

BroadcastManager::BroadcastManager(Config config, HttpClient& http_client)
    : config_(std::move(config)), http_client_(http_client) {}

BroadcastManager::~BroadcastManager() { stop_all(); }

std::shared_ptr<Broadcast> BroadcastManager::get_or_create(const std::string& infohash,
                                                           const std::map<std::string, std::string>& params) {
    // Claim the creation slot under the manager mutex, then build the Broadcast (which
    // blocks in AceClient::authenticate()) outside it, so a channel change no longer
    // serializes every other request. Concurrent callers for the same infohash wait on
    // the slot, which keeps the "one Broadcast per infohash" invariant.
    std::shared_ptr<PendingCreation> pending;
    std::shared_ptr<Broadcast> finished;
    bool creator = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = broadcasts_.find(infohash);
        if (existing != broadcasts_.end()) {
            if (!existing->second->finished()) return existing->second;
            // A broadcast whose stream thread gave up never sends data again; replace
            // it instead of attaching the new client to it.
            finished = existing->second;
            broadcasts_.erase(existing);
        }
        auto slot = creating_.find(infohash);
        if (slot == creating_.end()) {
            slot = creating_.emplace(infohash, std::make_shared<PendingCreation>()).first;
            creator = true;
        }
        pending = slot->second;
    }
    if (finished) finished->stop();

    if (!creator) {
        std::unique_lock<std::mutex> lock(pending->mutex);
        pending->cv.wait(lock, [&] { return pending->done; });
        if (pending->error) std::rethrow_exception(pending->error);
        if (!pending->broadcast) throw std::runtime_error("broadcast creation failed");
        return pending->broadcast;
    }

    std::shared_ptr<Broadcast> broadcast;
    try {
        broadcast = std::make_shared<Broadcast>(infohash, config_, http_client_, params);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            creating_.erase(infohash);
        }
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->error = std::current_exception();
            pending->done = true;
        }
        pending->cv.notify_all();
        throw;
    }

    bool stop_created = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            // Shutdown began while we were creating: do not keep a live session behind.
            stop_created = true;
        } else {
            broadcasts_[infohash] = broadcast;
        }
        creating_.erase(infohash);
    }
    if (stop_created) broadcast->stop();
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->broadcast = broadcast;
        pending->done = true;
    }
    pending->cv.notify_all();
    return broadcast;
}

std::shared_ptr<Broadcast> BroadcastManager::find(const std::string& infohash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = broadcasts_.find(infohash);
    return it == broadcasts_.end() ? nullptr : it->second;
}

void BroadcastManager::remove_if_empty(const std::string& infohash) {
    std::shared_ptr<Broadcast> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = broadcasts_.find(infohash);
        if (it != broadcasts_.end() && it->second->client_count() == 0) {
            removed = it->second;
            broadcasts_.erase(it);
        }
    }
    if (removed) removed->stop();
}

std::size_t BroadcastManager::broadcast_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return broadcasts_.size();
}

std::size_t BroadcastManager::client_count() const {
    std::size_t total = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, broadcast] : broadcasts_) total += broadcast->client_count();
    return total;
}

std::vector<std::shared_ptr<StreamClient>> BroadcastManager::all_clients() const {
    std::vector<std::shared_ptr<StreamClient>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, broadcast] : broadcasts_) {
        auto clients = broadcast->clients();
        out.insert(out.end(), clients.begin(), clients.end());
    }
    return out;
}

void BroadcastManager::stop_all() {
    std::map<std::string, std::shared_ptr<Broadcast>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        copy.swap(broadcasts_);
    }
    for (auto& [_, broadcast] : copy) broadcast->stop();
}

} // namespace httpace
