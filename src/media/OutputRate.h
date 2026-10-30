#pragma once

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace xdb::media {

// Accessed by the video worker only. Measure from the last actual publication;
// never accumulate credits or publish a catch-up burst after a pause.
class OutputRate {
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> last_;
public:
    bool due(unsigned fps, Clock::time_point now) const {
        return fps == 0 || !last_ ||
            std::chrono::duration<double>(now - *last_).count() >= 1.0 / fps;
    }
    void published(Clock::time_point now) { last_ = now; }
};

// A request belongs to a control group, not each of its attached RPC sockets.
// Guard with the session's media/control locks. Zero means unlimited and wins.
class OutputRateRequests {
    std::map<std::string, unsigned> requests_;
public:
    void set(const std::string &group, unsigned fps) { requests_[group] = fps; }
    void erase(const std::string &group) { requests_.erase(group); }
    void clear() { requests_.clear(); }
    unsigned effective() const {
        unsigned fps = 0;
        for (const auto &[group, requested] : requests_) {
            if (requested == 0) return 0;
            fps = std::max(fps, requested);
        }
        return fps;
    }
};
} // namespace xdb::media
