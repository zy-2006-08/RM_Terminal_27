#include "video.h"
#include <iostream>
#include <vector>

static std::vector<uint8_t> packet(uint16_t frame, uint16_t fragment, uint32_t total, const char* text, size_t size) {
    std::vector<uint8_t> p(8 + size);
    p[0]=uint8_t(frame>>8); p[1]=uint8_t(frame); p[2]=uint8_t(fragment>>8); p[3]=uint8_t(fragment);
    p[4]=uint8_t(total>>24); p[5]=uint8_t(total>>16); p[6]=uint8_t(total>>8); p[7]=uint8_t(total);
    for(size_t i=0;i<size;++i)p[8+i]=uint8_t(text[i]); return p;
}
static bool complete_out_of_order_and_duplicate() {
    rm_terminal::VideoReassembler r;
    std::string body(1392,'a'); body += "bc";
    auto second=packet(7,1,1394,body.data()+1392,2), first=packet(7,0,1394,body.data(),1392);
    if(r.push(second.data(),second.size(),0)||r.push(first.data(),first.size(),.1)!=std::vector<uint8_t>(body.begin(),body.end()))return false;
    r.push(first.data(),first.size(),.2); return r.stats().packets_duplicated==1;
}
static bool malformed_and_stale_are_counted() {
    rm_terminal::VideoReassembler r;
    uint8_t bad[7]{}; r.push(bad,sizeof(bad),0);
    std::string body(1392,'a');
    auto partial=packet(2,0,1394,body.data(),body.size()); r.push(partial.data(),partial.size(),0); r.expire(.31);
    return r.stats().malformed_packets==1&&r.stats().frames_dropped_stale==1;
}
static bool rejects_impossible_fragment_index() {
    rm_terminal::VideoReassembler r;
    auto invalid=packet(9,1,2,"ax",2);
    const auto frame=r.push(invalid.data(),invalid.size(),0);
    return !frame && r.stats().malformed_packets==1;
}
int main() {
    if(!rejects_impossible_fragment_index()){std::cerr<<"FAIL rejects_impossible_fragment_index\n";return 1;}
    return complete_out_of_order_and_duplicate()&&malformed_and_stale_are_counted()?0:1;
}
