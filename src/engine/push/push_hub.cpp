#include "push/push_hub.h"

#include <algorithm>

namespace fourfold {

std::string pushEvent(const std::string& event, const std::string& raw) {
    // A final line break would only add an empty line to the data.
    std::string data = raw;
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) data.pop_back();
    std::string out = "event: " + event + "\n";
    size_t start = 0;
    for (;;) {
        size_t end = data.find_first_of("\r\n", start);
        out += "data: " + data.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
        if (end == std::string::npos) break;
        start = end + (data.compare(end, 2, "\r\n") == 0 ? 2 : 1);
    }
    return out + "\n";
}

std::shared_ptr<PushHub> PushHub::create(EventLoop& loop, Limits limits) {
    return std::shared_ptr<PushHub>(new PushHub(loop, limits));
}

bool PushHub::full() const {
    std::lock_guard<std::mutex> lock(countsMutex_);
    return total_ >= limits_.maxSubscribers;
}

void PushHub::subscribe(const std::string& channel, std::shared_ptr<StreamWriter> writer) {
    // A comment first, so the browser knows the stream is open.
    if (!writer->write(": subscribed to " + channel + "\n\n")) return;
    channels_[channel].push_back(std::move(writer));
    updateCounts();
    startHeartbeat();
}

void PushHub::publish(const std::string& channel, const std::string& event, const std::string& data) {
    std::weak_ptr<PushHub> weak = weak_from_this();
    std::string message = pushEvent(event, data);
    loop_.post([weak, channel, message] {
        auto hub = weak.lock();
        if (!hub) return;
        auto it = hub->channels_.find(channel);
        if (it == hub->channels_.end()) return;
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const std::shared_ptr<StreamWriter>& w) { return !w->write(message); }),
                   list.end());
        if (list.empty()) hub->channels_.erase(it);
        hub->updateCounts();
    });
}

void PushHub::sweep() {
    for (auto it = channels_.begin(); it != channels_.end();) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [](const std::shared_ptr<StreamWriter>& w) { return !w->write(": ping\n\n"); }),
                   list.end());
        it = list.empty() ? channels_.erase(it) : std::next(it);
    }
    updateCounts();
}

void PushHub::startHeartbeat() {
    if (heartbeatRunning_) return;
    heartbeatRunning_ = true;
    std::weak_ptr<PushHub> weak = weak_from_this();
    loop_.addTimer(limits_.heartbeat, [weak] {
        auto hub = weak.lock();
        if (!hub) return;
        hub->heartbeatRunning_ = false;
        hub->sweep();
        if (!hub->channels_.empty()) hub->startHeartbeat();
    });
}

void PushHub::updateCounts() {
    std::lock_guard<std::mutex> lock(countsMutex_);
    counts_.clear();
    total_ = 0;
    for (const auto& [name, list] : channels_) {
        counts_[name] = list.size();
        total_ += list.size();
    }
}

std::map<std::string, size_t> PushHub::counts() const {
    std::lock_guard<std::mutex> lock(countsMutex_);
    return counts_;
}

size_t PushHub::total() const {
    std::lock_guard<std::mutex> lock(countsMutex_);
    return total_;
}

}  // namespace fourfold
