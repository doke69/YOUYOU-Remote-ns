#include "frame_trace.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

int main(int argc, char **argv) {
    static_assert(std::is_trivially_copyable_v<uu::PlayoutTraceRow>);
    static_assert(std::is_trivially_copyable_v<uu::ReceiverTraceRow>);
    std::ostringstream out;
    out << uu::PlayoutTraceRow{4294967295u,-1,0,1794414465193LL,1794414465193LL,-10,"settings_changed"};
    if(out.str()!="4294967295,-1,0,1794414465193,settings_changed,1794414465193,-10") return 1;
    out.str({});out.clear();
    out << uu::ReceiverTraceRow{4294967295u,4096,4194304,600,"missing_packets"};
    if(out.str()!="4294967295,4096,4194304,600,missing_packets") return 2;
    // Optional real-device CSV replay: preserve every existing column/value.
    for(int file=1;file<argc;++file) {
        std::ifstream input(argv[file]);
        std::string line;
        if(!std::getline(input,line)) return 3;
        const bool playout=line.rfind("media_timestamp_90khz,",0)==0;
        while(std::getline(input,line)) {
            if(!line.empty() && line.back()=='\r')line.pop_back();
            if(line.empty())continue;
            std::istringstream row(line);std::vector<std::string> fields;std::string field;
            while(std::getline(row,field,','))fields.push_back(field);
            if(fields.size()!=(playout?7u:5u))return 4;
            out.str({});out.clear();
            if(playout)out << uu::PlayoutTraceRow{uint32_t(std::stoul(fields[0])),
                std::stoll(fields[1]),std::stoll(fields[2]),std::stoll(fields[3]),
                std::stoll(fields[5]),std::stoll(fields[6]),fields[4].c_str()};
            else out << uu::ReceiverTraceRow{uint32_t(std::stoul(fields[0])),
                std::stoull(fields[1]),std::stoull(fields[2]),std::stoll(fields[3]),fields[4].c_str()};
            if(out.str()!=line)return 5;
        }
    }
    return 0;
}
