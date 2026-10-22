#include "rv_smaf_hps.h"
#include <string.h>

namespace {
static uint32_t be32(const uint8_t *b) {
    return (uint32_t(b[0])<<24)|(uint32_t(b[1])<<16)|
           (uint32_t(b[2])<<8)|uint32_t(b[3]);
}
static uint32_t timebase(uint8_t c) {
    switch(c) {
        case 0x00: return 1;
        case 0x01: return 2;
        case 0x02: return 4;
        case 0x03: return 5;
        case 0x10: return 10;
        case 0x11: return 20;
        case 0x12: return 40;
        case 0x13: return 50;
        default: return 0;
    }
}
// Handy Phone Standard duration uses a special two-byte variable length:
// >= 0x80 means ((first & 0x7f) + 1) * 128 + second. It is NOT MIDI VLQ.
static bool read_hps_duration(const uint8_t *b,size_t n,size_t &p,uint32_t &v) {
    if(p>=n) return false;
    const uint8_t first=b[p++];
    if(first<0x80) { v=first; return true; }
    if(p>=n) return false;
    v=((uint32_t(first&0x7f)+1u)<<7)|b[p++];
    return true;
}
struct Channel {
    uint8_t program, volume, expression, rhythm;
    int8_t octave_offset;
};
}

bool rv_smaf_hps_parse_track(const uint8_t *mtr,size_t len,
                             RvSmafHpsNoteFn callback,void *ctx,
                             uint32_t *out_end_ms,uint32_t *out_notes) {
    if(out_end_ms) *out_end_ms=0;
    if(out_notes) *out_notes=0;
    if(!mtr || len<6 || mtr[0]!=0x00) return false;
    const uint32_t td=timebase(mtr[2]),tg=timebase(mtr[3]);
    if(!td || !tg) return false;
    const uint8_t *seq=nullptr;
    size_t seq_len=0;
    // Format 0's header contains TWO status bytes. Mobile Format 2 has 16;
    // using a 20-byte offset here silently misses every Violet music track.
    for(size_t p=6;p+8<=len;) {
        uint32_t sz=be32(mtr+p+4);
        if(sz>len-p-8) return false;
        if(memcmp(mtr+p,"Mtsq",4)==0) { seq=mtr+p+8; seq_len=sz; break; }
        p+=size_t(sz)+8;
    }
    if(!seq || !seq_len) return false;
    Channel c[4]{};
    for(unsigned i=0;i<4;i++) {
        c[i].volume=127;
        c[i].expression=127;
        const uint8_t pair=mtr[4+i/2];
        const uint8_t status=(i&1)?(pair&15):(pair>>4);
        c[i].rhythm=((status&3)==3);
    }
    uint32_t end=0, count=0, now=0;
    size_t p=0;
    while(p<seq_len) {
        uint32_t delta=0;
        if(!read_hps_duration(seq,seq_len,p,delta)) return false;
        if((uint64_t)now+(uint64_t)delta*td>600000u) return false;
        now+=delta*td;
        if(p>=seq_len) return false;
        const uint8_t event=seq[p++];
        if(event==0xff) {
            if(p>=seq_len) return false;
            const uint8_t sub=seq[p++];
            if(sub==0) continue; // NOP
            uint32_t skip=0;
            if(sub==0xf0) {
                if(!read_hps_duration(seq,seq_len,p,skip)) return false;
            } else {
                if(p>=seq_len) return false;
                skip=seq[p++];
            }
            if(skip>seq_len-p) return false;
            p+=skip;
            continue;
        }
        if(event==0) {
            if(p>=seq_len) return false;
            uint8_t control=seq[p++];
            if(control==0) {
                if(p>=seq_len) return false;
                if(seq[p++]==0) break; // EOS 00 00 00
                continue;
            }
            const unsigned ch=control>>6;
            const unsigned cls=(control>>4)&3;
            const unsigned cmd=control&15;
            if(cls==3) {
                if(p>=seq_len) return false;
                const uint8_t value=seq[p++];
                switch(cmd) {
                    case 0: c[ch].program=value&127; break;
                    case 2:  // octave shift, encoded signed octaves
                        c[ch].octave_offset=(value>=0x81 && value<=0x84)?-(int)(value-0x80):
                                            (value>=1 && value<=4)?(int)value:0;
                        break;
                    case 7: c[ch].volume=value&127; break;
                    case 11:c[ch].expression=value&127; break;
                    default: break;
                }
            } else if(cls==0) {
                c[ch].expression=(cmd<=1)?0:((cmd*8+15)>127?127:(cmd*8+15));
            }
            continue;
        }
        const unsigned ch=event>>6;
        const uint8_t octave=(event>>4)&3;
        const uint8_t pitch=event&15;
        uint32_t gate=0;
        if(!read_hps_duration(seq,seq_len,p,gate)) return false;
        if(!gate) continue;
        const uint64_t duration=(uint64_t)gate*tg;
        if((uint64_t)now+duration>600000u) return false;
        const uint32_t end_note=now+(uint32_t)duration;
        if(end_note>end) end=end_note;
        const int midi=36+pitch+12*(int(octave)+c[ch].octave_offset);
        if(midi<0 || midi>=128) continue;
        const RvSmafHpsNote note={now,(uint32_t)duration,(uint8_t)midi,(uint8_t)ch,
                                  127,c[ch].program,c[ch].volume,c[ch].expression,c[ch].rhythm};
        if(callback) callback(ctx,&note);
        ++count;
    }
    if(out_end_ms) *out_end_ms=end>now?end:now;
    if(out_notes) *out_notes=count;
    return count>0;
}
