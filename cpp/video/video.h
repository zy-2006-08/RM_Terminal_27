#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace rm_terminal {
struct VideoStats {
    uint64_t packets_received=0, packets_duplicated=0, packets_out_of_order=0;
    uint64_t frames_completed=0, frames_dropped_incomplete=0, frames_dropped_stale=0;
    uint64_t malformed_packets=0, bytes_received=0, packets_missing=0, stale_packets=0;
};
class VideoReassembler final {
public:
    explicit VideoReassembler(double timeout_seconds=.30, size_t cache_max=8);
    std::optional<std::vector<uint8_t>> push(const uint8_t* data, size_t size, double now);
    void expire(double now);
    void resetSession();
    const VideoStats& stats() const { return stats_; }
    size_t cachedFrames() const { return partial_?1:0; }
private:
    struct Partial {
        uint32_t total;
        double first;
        std::map<uint16_t,std::vector<uint8_t>> chunks;
    };
    static bool newer(uint16_t a, uint16_t b);
    void drop(bool timeout);
    double timeout_;
    std::optional<Partial> partial_;
    std::optional<uint16_t> newest_, completed_;
    VideoStats stats_;
};
}
