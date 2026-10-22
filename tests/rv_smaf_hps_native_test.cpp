#include "../source/rv_smaf_hps.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <cstring>
static uint32_t be32(const uint8_t *b) {
    return (uint32_t(b[0])<<24)|(uint32_t(b[1])<<16)|
           (uint32_t(b[2])<<8)|b[3];
}
struct Stats { unsigned calls=0, notes=0; uint32_t maxend=0; bool bad=false; };
static void accept(void *ctx,const RvSmafHpsNote *n) {
    Stats &s=*(Stats*)ctx;
    s.calls++;
    if(n->note>=128 || n->channel>3 || n->start_ms+n->gate_ms>600000u) s.bad=true;
    s.maxend=std::max(s.maxend,n->start_ms+n->gate_ms);
}
int main(int argc,char **argv) {
    assert(argc>1);
    int themes=0, scored_files=0, total_notes=0;
    for(int id=0;id<=25;id++) {
        const std::string name=std::string(argv[1])+"/res/sound/"+std::to_string(id)+".mmf";
        std::ifstream in(name,std::ios::binary);
        assert(in.good());
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)),{});
        assert(b.size()>16 && !memcmp(b.data(),"MMMD",4));
        uint32_t score_tracks=0;
        for(size_t p=8;p+8<=b.size();) {
            uint32_t size=be32(b.data()+p+4);
            assert(size<=b.size()-p-8);
            if(!memcmp(b.data()+p,"MTR",3)) {
                assert(size>=6 && b[p+8]==0);
                uint32_t len=0,n=0;
                assert(rv_smaf_hps_parse_track(b.data()+p+8,size,nullptr,nullptr,&len,&n));
                Stats st;
                assert(rv_smaf_hps_parse_track(b.data()+p+8,size,accept,&st,nullptr,nullptr));
                assert(!st.bad && n==st.calls && len>=st.maxend && n>0);
                score_tracks++; total_notes+=n;
            }
            p+=size+8;
        }
        if(score_tracks) { scored_files++; printf("%-6s: %u tracks\n",name.c_str()+name.size()-5,score_tracks); }
        if(id<=8) { assert(score_tracks>=3); themes++; }
    }
    assert(themes==9 && scored_files==14 && total_notes>2000);
    puts("PASS: all 9 multitrack themes + 5 short HPS cues parse and emit valid notes");
    return 0;
}
