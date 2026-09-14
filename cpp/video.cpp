#include "video.h"
#include <stdexcept>

namespace rm_terminal {
VideoReassembler::VideoReassembler(double timeout, size_t max):timeout_(timeout) {
    if(timeout<=0 || max==0)throw std::invalid_argument("positive video limits required");
}
bool VideoReassembler::newer(uint16_t a,uint16_t b) {
    return a!=b && static_cast<uint16_t>(a-b)<0x8000;
}
void VideoReassembler::drop(bool timeout) {
    if(!partial_)return;
    stats_.packets_missing+=(partial_->total+1391)/1392-partial_->chunks.size();
    if(timeout)++stats_.frames_dropped_stale;
    else ++stats_.frames_dropped_incomplete;
    partial_.reset();
}
void VideoReassembler::expire(double now) {
    if(partial_ && now-partial_->first>=timeout_)drop(true);
}
void VideoReassembler::resetSession() {
    drop(true); newest_.reset(); completed_.reset();
}
std::optional<std::vector<uint8_t>> VideoReassembler::push(const uint8_t* d,size_t n,double now) {
    expire(now);
    ++stats_.packets_received; stats_.bytes_received+=n;
    if(n<=8 || n>1400){++stats_.malformed_packets;return std::nullopt;}
    const uint16_t id=uint16_t(d[0]<<8|d[1]), ix=uint16_t(d[2]<<8|d[3]);
    const uint32_t total=uint32_t(d[4])<<24|uint32_t(d[5])<<16|uint32_t(d[6])<<8|d[7];
    // A 4 MiB access-unit bound prevents hostile headers from reserving arbitrary memory.
    if(!total || total>4u*1024u*1024u){++stats_.malformed_packets;return std::nullopt;}
    const size_t count=(total+1391)/1392;
    if(ix>=count || n-8!=(ix+1<count?1392:total-1392*(count-1))) {
        ++stats_.malformed_packets;return std::nullopt;
    }
    if(completed_ && id==*completed_){++stats_.packets_duplicated;return std::nullopt;}
    if(newest_ && id!=*newest_ && !newer(id,*newest_)) {
        ++stats_.stale_packets;return std::nullopt;
    }
    if(!newest_ || id!=*newest_) {
        drop(false); newest_=id; partial_=Partial{total,now,{}};
    } else if(!partial_) {
        ++stats_.stale_packets;return std::nullopt;
    }
    auto& p=*partial_;
    if(p.total!=total){++stats_.malformed_packets;return std::nullopt;}
    const auto found=p.chunks.find(ix);
    const std::vector<uint8_t> chunk(d+8,d+n);
    if(found!=p.chunks.end()) {
        if(found->second==chunk)++stats_.packets_duplicated;
        else ++stats_.malformed_packets;
        return std::nullopt;
    }
    if(ix!=p.chunks.size())++stats_.packets_out_of_order;
    p.chunks.emplace(ix,chunk);
    if(p.chunks.size()!=count)return std::nullopt;
    std::vector<uint8_t> out;out.reserve(total);
    for(const auto& entry:p.chunks)out.insert(out.end(),entry.second.begin(),entry.second.end());
    partial_.reset();completed_=id;++stats_.frames_completed;return out;
}
}
