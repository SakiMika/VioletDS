#include "zen_sav_backend.h"

#include <fat.h>
#ifndef RV_CART_SAVE_PROFILE
#define RV_CART_SAVE_PROFILE 0
#endif
#if RV_CART_SAVE_PROFILE
#include <nds.h>
#include <nds/card.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

namespace {
// ---------------------------------------------------------------------------
// Ragnarok Violet generic WIPI database persistence.
// ---------------------------------------------------------------------------
// The original Zenonia port hard-coded four record names into a 128 KiB
// ZLFT.sav image. Ragnarok uses three character files "save%d", plus "option" and the slot-summary "list".
// Keep the WIPI layer generic and persist named DBs in one FAT sidecar instead.
//
// Layout:
//   000000-000fff : global header / metadata area (4 KiB)
//   001000-020fff : physical COW record 0 (128 KiB block)
//   ...
//   1e1000-201fff : physical COW record 15
// Game-visible character slots are save0/save1/save2; option and list are metadata.
//
// Stores are copy-on-write when a free slot exists: write the new payload and
// header first, commit the global generation, then retire older copies. A reset
// during a save therefore leaves either the previous generation or the new one
// readable. Native Slot-1 EEPROM probing stays disabled for flashcart safety.
constexpr uint32_t SAV_VERSION         = 1u;
constexpr uint32_t SAV_META_SIZE       = 0x1000u;
constexpr uint32_t RAGNAROK_SAVE_SLOTS = 3u;   // game UI: save0, save1, save2
constexpr uint32_t PERSISTENT_NAMES    = 5u;   // three saves + option + slot-summary list
// FAT_RECORD_CAPACITY is copy-on-write storage capacity, NOT game save slots.
// Keep 16 physical record blocks so interrupted DLDI saves can retain an older
// generation without changing the game-visible 3-slot model.
constexpr uint32_t FAT_RECORD_CAPACITY = 16u;
constexpr uint32_t SAV_SLOT_BLOCK      = 0x20000u; // 128 KiB / physical COW record
constexpr uint32_t SAV_RECORD_HEADER   = 96u;
constexpr uint32_t SAV_TOTAL_SIZE      = SAV_META_SIZE + FAT_RECORD_CAPACITY*SAV_SLOT_BLOCK;
constexpr uint32_t SAV_NAME_MAX        = 47u;
constexpr uint32_t RECORD_FLAG_DELETED = 1u;

const unsigned char GLOBAL_MAGIC[8] = {'R','V','D','B','S','V','0','1'};
const unsigned char RECORD_MAGIC[8] = {'R','V','D','B','R','E','C','1'};

bool g_fat_available=false;
char g_fat_path[96]="";
bool g_native_available=false;

static const char *basename_of(const char *name) {
    if(!name) return "";
    const char *base=name;
    for(const char *p=name; *p; ++p) if(*p=='/' || *p=='\\') base=p+1;
    return base;
}

static char ascii_lower(char c) {
    return (c>='A' && c<='Z') ? (char)(c+('a'-'A')) : c;
}
static bool ascii_ieq(const char *a,const char *b) {
    if(!a || !b) return false;
    while(*a && *b) {
        if(ascii_lower(*a)!=ascii_lower(*b)) return false;
        ++a; ++b;
    }
    return *a==0 && *b==0;
}
static int save_slot_index(const char *base) {
    if(!base) return -1;
    if(ascii_lower(base[0])!='s' || ascii_lower(base[1])!='a' ||
       ascii_lower(base[2])!='v' || ascii_lower(base[3])!='e') return -1;
    const char *p=base+4;
    if(*p<'0' || *p>'9') return -1;
    unsigned n=0;
    while(*p>='0' && *p<='9') { n=n*10u+(unsigned)(*p-'0'); ++p; }
    if(!(*p==0 || ascii_ieq(p,".dat") || ascii_ieq(p,".sav"))) return -1;
    return n<RAGNAROK_SAVE_SLOTS ? (int)n : -1;
}
static bool is_save_number_name(const char *base) { return save_slot_index(base)>=0; }
static bool is_persistent_name(const char *name) {
    const char *base=basename_of(name);
    if(!base[0] || strlen(base)>SAV_NAME_MAX) return false;
    // Ragnarok Violet has exactly three character slots: save0/save1/save2.
    // "option" stores global settings and "list" stores the three-slot summary
    // used by the title screen. Losing list makes every slot appear New Game.
    return is_save_number_name(base) || ascii_ieq(base,"option") ||
           ascii_ieq(base,"option.sav") || ascii_ieq(base,"list");
}

static void put32(unsigned char *p,uint32_t v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint32_t crc32_bytes(const void *data,uint32_t len) {
    const unsigned char *p=(const unsigned char*)data;
    uint32_t crc=0xffffffffu;
    for(uint32_t i=0;i<len;i++) {
        crc^=p[i];
        for(int b=0;b<8;b++) crc=(crc>>1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc&1u));
    }
    return crc^0xffffffffu;
}


// ---------------------------------------------------------------------------
// Ragnarok Violet cartridge-save profile.
//
// IMPORTANT: stock melonDS chooses DS cartridge save geometry from its built-in
// ROM list.  An arbitrary code such as RVDS falls back to 8 KiB EEPROM, which
// is too small and also uses a different SPI address width.  The shipped ROM
// therefore uses compatibility game code CDLG, a stock ROM-list entry with
// 128 KiB EEPROM.  The project/port identifier remains RVDS in source/docs.
// ---------------------------------------------------------------------------
// This backend is compiled only with RV_CART_SAVE_PROFILE=1 and implements the
// game's 128 KiB EEPROM container (save0/save1/save2/option/list).
#if RV_CART_SAVE_PROFILE
constexpr uint32_t NATIVE_TOTAL_SIZE   = 128u*1024u;
constexpr uint32_t NATIVE_META_SIZE    = 0x1000u;
constexpr uint32_t NATIVE_ENTRY_COUNT  = PERSISTENT_NAMES;
constexpr uint32_t NATIVE_LEGACY_COUNT = 16u; // compatibility with earlier runner .sav files
constexpr uint32_t NATIVE_ENTRY_SIZE   = 64u;
constexpr uint32_t NATIVE_ENTRY_BASE   = 64u;
constexpr uint32_t NATIVE_META_CRC_OFF = NATIVE_META_SIZE-4u;
constexpr uint32_t NATIVE_PAGE_SIZE    = 128u;
const unsigned char NATIVE_MAGIC[8]={'R','V','N','D','S','V','0','1'};

static bool wait_aux_spi_idle(uint32_t budget=200000u) {
    while((REG_AUXSPICNT & CARD_SPI_BUSY) && budget) --budget;
    return (REG_AUXSPICNT & CARD_SPI_BUSY)==0;
}
static bool spi_byte_mode(unsigned char value,unsigned char *result,bool hold) {
    REG_AUXSPICNT = CARD_ENABLE | CARD_SPI_ENABLE | (hold ? CARD_SPI_HOLD : 0);
    REG_AUXSPIDATA = value;
    if(!wait_aux_spi_idle()) return false;
    if(result) *result=REG_AUXSPIDATA;
    return true;
}
static bool spi_hold(unsigned char value,unsigned char *result=nullptr) {
    return spi_byte_mode(value,result,true);
}
static bool spi_release(unsigned char value,unsigned char *result=nullptr) {
    const bool ok=spi_byte_mode(value,result,false);
    REG_AUXSPICNT=CARD_ENABLE;
    return ok;
}
static bool eeprom_write_enable() { return spi_release(0x06); }
static bool eeprom_wait_ready() {
    for(uint32_t poll=0;poll<8192u;poll++) {
        unsigned char status=0xff;
        if(!spi_hold(0x05) || !spi_release(0,&status)) return false;
        if((status&1u)==0) return true;
    }
    return false;
}
static bool native_read_bytes(uint32_t address,void *out,uint32_t len) {
    unsigned char *data=(unsigned char*)out;
    if(!data || address>NATIVE_TOTAL_SIZE || len>NATIVE_TOTAL_SIZE-address) return false;
    while(len) {
        const uint32_t chunk=len>1024u?1024u:len;
        bool ok=spi_hold(0x03) &&
                spi_hold((unsigned char)(address>>16)) &&
                spi_hold((unsigned char)(address>>8)) &&
                spi_hold((unsigned char)address);
        for(uint32_t i=0;ok && i<chunk;i++)
            ok=(i+1u==chunk)?spi_release(0,&data[i]):spi_hold(0,&data[i]);
        if(!ok) return false;
        address+=chunk; data+=chunk; len-=chunk;
    }
    return true;
}
static bool eeprom_write_page(uint32_t address,const unsigned char *data,uint32_t len) {
    if(!data || !len || len>NATIVE_PAGE_SIZE) return false;
    if(address>NATIVE_TOTAL_SIZE || len>NATIVE_TOTAL_SIZE-address) return false;
    if(((address&(NATIVE_PAGE_SIZE-1u))+len)>NATIVE_PAGE_SIZE) return false;
    if(!eeprom_write_enable()) return false;
    bool ok=spi_hold(0x02) &&
            spi_hold((unsigned char)(address>>16)) &&
            spi_hold((unsigned char)(address>>8)) &&
            spi_hold((unsigned char)address);
    for(uint32_t i=0;ok && i<len;i++)
        ok=(i+1u==len)?spi_release(data[i]):spi_hold(data[i]);
    return ok && eeprom_wait_ready();
}
static bool native_write_bytes(uint32_t address,const void *src,uint32_t len) {
    const unsigned char *data=(const unsigned char*)src;
    if((len && !data) || address>NATIVE_TOTAL_SIZE || len>NATIVE_TOTAL_SIZE-address) return false;
    while(len) {
        const uint32_t room=NATIVE_PAGE_SIZE-(address&(NATIVE_PAGE_SIZE-1u));
        const uint32_t chunk=len<room?len:room;
        if(!eeprom_write_page(address,data,chunk)) return false;
        address+=chunk; data+=chunk; len-=chunk;
    }
    return true;
}
static bool native_verify(uint32_t address,const void *expected,uint32_t len) {
    const unsigned char *src=(const unsigned char*)expected;
    unsigned char temp[128];
    while(len) {
        const uint32_t chunk=len>sizeof(temp)?sizeof(temp):len;
        if(!native_read_bytes(address,temp,chunk) || memcmp(temp,src,chunk)!=0) return false;
        address+=chunk; src+=chunk; len-=chunk;
    }
    return true;
}

struct NativeEntry {
    bool valid=false;
    char name[48]{};
    uint32_t offset=0;
    uint32_t len=0;
    uint32_t crc=0;
};

static unsigned char *native_entry_ptr(unsigned char meta[NATIVE_META_SIZE],uint32_t index) {
    return meta+NATIVE_ENTRY_BASE+index*NATIVE_ENTRY_SIZE;
}
static const unsigned char *native_entry_ptr_const(const unsigned char meta[NATIVE_META_SIZE],uint32_t index) {
    return meta+NATIVE_ENTRY_BASE+index*NATIVE_ENTRY_SIZE;
}
static void native_meta_init(unsigned char meta[NATIVE_META_SIZE],uint32_t generation=0) {
    memset(meta,0xff,NATIVE_META_SIZE);
    memcpy(meta,NATIVE_MAGIC,8);
    put32(meta+8,SAV_VERSION);
    put32(meta+12,generation);
    put32(meta+16,NATIVE_ENTRY_COUNT);
    put32(meta+20,NATIVE_META_SIZE);
    put32(meta+24,NATIVE_META_SIZE);
    put32(meta+28,0x31564e52u); // "RNV1"
    put32(meta+NATIVE_META_CRC_OFF,crc32_bytes(meta,NATIVE_META_CRC_OFF));
}
static uint32_t native_meta_count(const unsigned char meta[NATIVE_META_SIZE]) {
    const uint32_t count=get32(meta+16);
    return (count==NATIVE_ENTRY_COUNT || count==NATIVE_LEGACY_COUNT) ? count : 0u;
}
static bool native_parse_entry(const unsigned char meta[NATIVE_META_SIZE],uint32_t index,NativeEntry *out) {
    const uint32_t count=native_meta_count(meta);
    if(!out || !count || index>=count) return false;
    const unsigned char *e=native_entry_ptr_const(meta,index);
    if(e[0]==0xff || e[0]==0) return false;
    size_t name_len=0; while(name_len<48u && e[name_len]!=0) ++name_len;
    if(name_len==0 || name_len>=48u) return false;
    const uint32_t off=get32(e+48), len=get32(e+52), crc=get32(e+56), flags=get32(e+60);
    if(flags!=0 || off<NATIVE_META_SIZE || len>NATIVE_TOTAL_SIZE-NATIVE_META_SIZE || off>NATIVE_TOTAL_SIZE-len) return false;
    *out=NativeEntry{}; out->valid=true;
    memcpy(out->name,e,name_len); out->name[name_len]=0;
    out->offset=off; out->len=len; out->crc=crc;
    return true;
}
static void native_write_entry(unsigned char meta[NATIVE_META_SIZE],uint32_t index,const char *name,uint32_t off,uint32_t len,uint32_t crc) {
    unsigned char *e=native_entry_ptr(meta,index); memset(e,0xff,NATIVE_ENTRY_SIZE);
    const size_t n=strlen(name); memcpy(e,name,n); e[n]=0;
    put32(e+48,off); put32(e+52,len); put32(e+56,crc); put32(e+60,0);
}
static bool native_meta_valid(const unsigned char meta[NATIVE_META_SIZE]) {
    const uint32_t count=native_meta_count(meta);
    if(memcmp(meta,NATIVE_MAGIC,8)!=0 || get32(meta+8)!=SAV_VERSION ||
       !count || get32(meta+20)!=NATIVE_META_SIZE) return false;
    if(get32(meta+NATIVE_META_CRC_OFF)!=crc32_bytes(meta,NATIVE_META_CRC_OFF)) return false;
    for(uint32_t i=0;i<count;i++) {
        const unsigned char *e=native_entry_ptr_const(meta,i);
        if(e[0]==0xff || e[0]==0) continue;
        NativeEntry tmp{}; if(!native_parse_entry(meta,i,&tmp)) return false;
    }
    return true;
}
static bool native_read_meta(unsigned char meta[NATIVE_META_SIZE]) {
    return native_read_bytes(0,meta,NATIVE_META_SIZE) && native_meta_valid(meta);
}
static bool native_write_meta(unsigned char meta[NATIVE_META_SIZE]) {
    put32(meta+NATIVE_META_CRC_OFF,crc32_bytes(meta,NATIVE_META_CRC_OFF));
    return native_write_bytes(0,meta,NATIVE_META_SIZE) && native_verify(0,meta,NATIVE_META_SIZE);
}
static bool native_probe_hw() {
    unsigned char probe[16];
    return native_read_bytes(0,probe,sizeof(probe));
}
static int native_find(const unsigned char meta[NATIVE_META_SIZE],const char *name,NativeEntry *out=nullptr) {
    const char *base=basename_of(name);
    const uint32_t count=native_meta_count(meta);
    for(uint32_t i=0;i<count;i++) {
        NativeEntry e{};
        if(native_parse_entry(meta,i,&e) && !strcmp(e.name,base)) { if(out) *out=e; return (int)i; }
    }
    return -1;
}
static bool native_record_exists(const char *name,uint32_t *out_len) {
    unsigned char meta[NATIVE_META_SIZE]; if(!g_native_available || !native_read_meta(meta)) return false;
    NativeEntry e{}; if(native_find(meta,name,&e)<0) return false;
    if(out_len) *out_len=e.len;
    return true;
}
static int native_load_record(const char *name,unsigned char **out,uint32_t *out_len) {
    if(!out || !out_len || !g_native_available) return -12;
    *out=nullptr; *out_len=0;
    unsigned char meta[NATIVE_META_SIZE]; if(!native_read_meta(meta)) return -12;
    NativeEntry e{}; if(native_find(meta,name,&e)<0) return -12;
    unsigned char *mem=(unsigned char*)malloc(e.len?e.len:1u); if(!mem) return -3;
    if(e.len && !native_read_bytes(e.offset,mem,e.len)) { free(mem); return -12; }
    if(e.crc!=crc32_bytes(mem,e.len)) { free(mem); return -12; }
    *out=mem; *out_len=e.len;
    printf("[SAV] EEPROM load %s len=%lu\n",basename_of(name),(unsigned long)e.len);
    return 0;
}

// Repack the latest named records into the 124 KiB payload area.  This profile
// is emulator-facing; DLDI/hardware builds never compile this path.  Metadata
// is written last so a completed save always has self-consistent offsets/CRCs.
static int native_repack_record(const char *name,const unsigned char *data,uint32_t len,bool remove_target) {
    if(!g_native_available || !is_persistent_name(name)) return -12;
    if((len && !data) || strlen(basename_of(name))>SAV_NAME_MAX) return -9;
    unsigned char old_meta[NATIVE_META_SIZE];
    if(!native_read_meta(old_meta)) native_meta_init(old_meta,0);
    const uint32_t old_generation=native_meta_valid(old_meta)?get32(old_meta+12):0;
    const uint32_t payload_cap=NATIVE_TOTAL_SIZE-NATIVE_META_SIZE;
    unsigned char *payload=(unsigned char*)malloc(payload_cap?payload_cap:1u); if(!payload) return -3;
    NativeEntry keep[NATIVE_ENTRY_COUNT]{}; uint32_t keep_count=0, cursor=0;
    const char *base=basename_of(name);
    bool found_target=false;
    const uint32_t old_count=native_meta_count(old_meta);
    for(uint32_t i=0;i<old_count;i++) {
        NativeEntry e{}; if(!native_parse_entry(old_meta,i,&e)) continue;
        if(!is_persistent_name(e.name)) continue; // drop legacy save3..save15 entries
        if(!strcmp(e.name,base)) { found_target=true; continue; }
        if(keep_count>=NATIVE_ENTRY_COUNT || e.len>payload_cap-cursor) { free(payload); return -18; }
        if(e.len && !native_read_bytes(e.offset,payload+cursor,e.len)) { free(payload); return -3; }
        if(e.crc!=crc32_bytes(payload+cursor,e.len)) {
            printf("[SAV!] EEPROM CRC skip %s\n",e.name); continue;
        }
        keep[keep_count]=e; keep[keep_count].offset=NATIVE_META_SIZE+cursor;
        cursor+=e.len; keep_count++;
    }
    if(!remove_target) {
        if(keep_count>=NATIVE_ENTRY_COUNT || len>payload_cap-cursor) {
            printf("[SAV!] EEPROM container full for %s len=%lu used=%lu cap=%lu\n",base,(unsigned long)len,(unsigned long)cursor,(unsigned long)payload_cap);
            free(payload); return -18;
        }
        if(len) memcpy(payload+cursor,data,len);
        NativeEntry &e=keep[keep_count++]; e.valid=true; strncpy(e.name,base,sizeof(e.name)-1);
        e.offset=NATIVE_META_SIZE+cursor; e.len=len; e.crc=crc32_bytes(data,len); cursor+=len;
    } else if(!found_target) { free(payload); return -12; }

    unsigned char meta[NATIVE_META_SIZE]; native_meta_init(meta,old_generation+1u);
    put32(meta+24,NATIVE_META_SIZE+cursor);
    for(uint32_t i=0;i<keep_count;i++) native_write_entry(meta,i,keep[i].name,keep[i].offset,keep[i].len,keep[i].crc);
    put32(meta+NATIVE_META_CRC_OFF,crc32_bytes(meta,NATIVE_META_CRC_OFF));

    if(cursor && !native_write_bytes(NATIVE_META_SIZE,payload,cursor)) { free(payload); return -3; }
    free(payload);
    if(!native_write_meta(meta)) return -3;
    printf("[SAV] EEPROM %s %s len=%lu total=%lu gen=%lu\n",remove_target?"remove":"write",base,(unsigned long)len,(unsigned long)cursor,(unsigned long)(old_generation+1u));
    return 0;
}
static int native_store_record(const char *name,const unsigned char *data,uint32_t len) {
    return native_repack_record(name,data,len,false);
}
static int native_remove_record(const char *name) {
    return native_repack_record(name,nullptr,0,true);
}
#endif // RV_CART_SAVE_PROFILE
static uint32_t record_offset(int id) {
    return SAV_META_SIZE + (uint32_t)id*SAV_SLOT_BLOCK;
}
static uint32_t record_payload_max() {
    return SAV_SLOT_BLOCK-SAV_RECORD_HEADER;
}

static bool fat_probe_prefix(const char *prefix) {
    char path[96];
    snprintf(path,sizeof(path),"%sRV_probe.tmp",prefix?prefix:"");
    FILE *f=fopen(path,"wb");
    if(!f) return false;
    static const unsigned char marker[4]={'R','V','D','B'};
    bool ok=fwrite(marker,1,sizeof(marker),f)==sizeof(marker);
    if(ok) ok=fflush(f)==0;
    if(fclose(f)!=0) ok=false;
    if(ok) remove(path);
    return ok;
}

static bool global_header_valid(FILE *f,uint32_t *generation=nullptr) {
    if(!f || fseek(f,0,SEEK_SET)!=0) return false;
    unsigned char h[64];
    if(fread(h,1,sizeof(h),f)!=sizeof(h)) return false;
    if(memcmp(h,GLOBAL_MAGIC,8)!=0) return false;
    if(get32(h+8)!=SAV_VERSION || get32(h+12)!=SAV_TOTAL_SIZE ||
       get32(h+16)!=FAT_RECORD_CAPACITY || get32(h+20)!=SAV_SLOT_BLOCK) return false;
    if(get32(h+60)!=crc32_bytes(h,60)) return false;
    if(generation) *generation=get32(h+24);
    return true;
}
static bool write_global_header(FILE *f,uint32_t generation) {
    if(!f) return false;
    unsigned char h[64]; memset(h,0xff,sizeof(h));
    memcpy(h,GLOBAL_MAGIC,8);
    put32(h+8,SAV_VERSION); put32(h+12,SAV_TOTAL_SIZE);
    put32(h+16,FAT_RECORD_CAPACITY); put32(h+20,SAV_SLOT_BLOCK);
    put32(h+24,generation); put32(h+28,0x31425652u); // "RVB1"
    put32(h+60,crc32_bytes(h,60));
    return fseek(f,0,SEEK_SET)==0 && fwrite(h,1,sizeof(h),f)==sizeof(h);
}
static bool init_container(FILE *f) {
    if(!f) return false;
    if(fseek(f,(long)SAV_TOTAL_SIZE-1,SEEK_SET)!=0) return false;
    const unsigned char ff=0xff;
    if(fwrite(&ff,1,1,f)!=1) return false;
    return write_global_header(f,0) && fflush(f)==0;
}
static FILE *open_rw() {
    if(!g_fat_available || !g_fat_path[0]) return nullptr;
    FILE *f=fopen(g_fat_path,"r+b");
    if(!f) f=fopen(g_fat_path,"w+b");
    if(!f) return nullptr;
    if(!global_header_valid(f)) {
        fclose(f);
        f=fopen(g_fat_path,"w+b");
        if(!f || !init_container(f)) { if(f) fclose(f); return nullptr; }
    }
    return f;
}
static FILE *open_ro() {
    if(!g_fat_available || !g_fat_path[0]) return nullptr;
    FILE *f=fopen(g_fat_path,"rb");
    if(!f) return nullptr;
    if(!global_header_valid(f)) { fclose(f); return nullptr; }
    return f;
}

struct RecordInfo {
    bool valid=false;
    int id=-1;
    uint32_t generation=0;
    uint32_t len=0;
    char name[48]{};
};

static bool read_record_header(FILE *f,int id,RecordInfo *out) {
    if(!f || id<0 || id>=(int)FAT_RECORD_CAPACITY || !out) return false;
    unsigned char h[SAV_RECORD_HEADER];
    if(fseek(f,(long)record_offset(id),SEEK_SET)!=0 || fread(h,1,sizeof(h),f)!=sizeof(h)) return false;
    if(memcmp(h,RECORD_MAGIC,8)!=0 || get32(h+8)!=SAV_VERSION) return false;
    const uint32_t generation=get32(h+12);
    const uint32_t len=get32(h+16);
    const uint32_t name_len=get32(h+24);
    const uint32_t flags=get32(h+28);
    if(flags&RECORD_FLAG_DELETED) return false;
    if(len>record_payload_max() || name_len==0 || name_len>SAV_NAME_MAX) return false;
    if(get32(h+92)!=crc32_bytes(h,92)) return false;
    char name[48]; memset(name,0,sizeof(name)); memcpy(name,h+32,name_len);
    if(name[name_len]!=0) return false;
    out->valid=true; out->id=id; out->generation=generation; out->len=len;
    memcpy(out->name,name,sizeof(out->name));
    return true;
}
static int find_latest(FILE *f,const char *name,RecordInfo *out,int *first_free=nullptr) {
    if(first_free) *first_free=-1;
    RecordInfo best{};
    for(int id=0;id<(int)FAT_RECORD_CAPACITY;id++) {
        RecordInfo r{};
        if(!read_record_header(f,id,&r)) {
            if(first_free && *first_free<0) *first_free=id;
            continue;
        }
        if(name && !strcmp(r.name,basename_of(name)) && (!best.valid || r.generation>best.generation)) best=r;
    }
    if(!best.valid) return -12;
    if(out) *out=best;
    return 0;
}
static bool write_record_header(FILE *f,int id,const char *name,const unsigned char *data,uint32_t len,uint32_t generation) {
    const char *base=basename_of(name);
    const size_t name_len=strlen(base);
    if(!f || id<0 || id>=(int)FAT_RECORD_CAPACITY || name_len==0 || name_len>SAV_NAME_MAX || len>record_payload_max()) return false;
    unsigned char h[SAV_RECORD_HEADER]; memset(h,0xff,sizeof(h));
    memcpy(h,RECORD_MAGIC,8); put32(h+8,SAV_VERSION); put32(h+12,generation);
    put32(h+16,len); put32(h+20,crc32_bytes(data,len)); put32(h+24,(uint32_t)name_len); put32(h+28,0);
    memcpy(h+32,base,name_len); h[32+name_len]=0;
    put32(h+92,crc32_bytes(h,92));
    return fseek(f,(long)record_offset(id),SEEK_SET)==0 && fwrite(h,1,sizeof(h),f)==sizeof(h);
}
static bool clear_record_header(FILE *f,int id) {
    unsigned char blank[SAV_RECORD_HEADER]; memset(blank,0xff,sizeof(blank));
    return fseek(f,(long)record_offset(id),SEEK_SET)==0 && fwrite(blank,1,sizeof(blank),f)==sizeof(blank);
}
static bool sync_file(FILE *f) {
    if(!f || fflush(f)!=0) return false;
    const int fd=fileno(f);
    return fd<0 || fsync(fd)==0;
}

static bool record_exists(const char *name,uint32_t *out_len) {
    FILE *f=open_ro(); if(!f) return false;
    RecordInfo r{}; const int rc=find_latest(f,name,&r,nullptr);
    fclose(f);
    if(rc!=0) return false;
    if(out_len) *out_len=r.len;
    return true;
}
static int load_record(const char *name,unsigned char **out,uint32_t *out_len) {
    if(!out || !out_len) return -9;
    *out=nullptr; *out_len=0;
    FILE *f=open_ro(); if(!f) return -12;
    const char *base=basename_of(name);
    unsigned char *best_mem=nullptr;
    uint32_t best_len=0, best_generation=0;
    bool found=false;
    // Verify payload CRC while selecting the newest generation. If a reset
    // interrupted a copy-on-write update, an incomplete newer copy is ignored
    // and the previous generation remains loadable.
    for(int id=0;id<(int)FAT_RECORD_CAPACITY;id++) {
        RecordInfo r{};
        if(!read_record_header(f,id,&r) || strcmp(r.name,base)!=0) continue;
        unsigned char h[SAV_RECORD_HEADER];
        if(fseek(f,(long)record_offset(id),SEEK_SET)!=0 || fread(h,1,sizeof(h),f)!=sizeof(h)) continue;
        unsigned char *mem=(unsigned char*)malloc(r.len?r.len:1);
        if(!mem) { free(best_mem); fclose(f); return -3; }
        bool ok=!r.len || fread(mem,1,r.len,f)==r.len;
        if(ok) ok=get32(h+20)==crc32_bytes(mem,r.len);
        if(!ok) { free(mem); continue; }
        if(!found || r.generation>best_generation) {
            free(best_mem); best_mem=mem; best_len=r.len; best_generation=r.generation; found=true;
        } else free(mem);
    }
    fclose(f);
    if(!found) return -12;
    *out=best_mem; *out_len=best_len;
    return 0;
}
static int store_record(const char *name,const unsigned char *data,uint32_t len) {
    if(!is_persistent_name(name)) return -12;
    if(len && !data) return -9;
    if(len>record_payload_max()) {
        printf("[SAV!] %s too large %lu > %lu\n",basename_of(name),(unsigned long)len,(unsigned long)record_payload_max());
        return -18;
    }
    FILE *f=open_rw(); if(!f) return -3;
    uint32_t global_generation=0; global_header_valid(f,&global_generation);
    RecordInfo previous{}; int free_id=-1;
    const int previous_rc=find_latest(f,name,&previous,&free_id);
    if(free_id<0) {
        // All slots valid. Reuse the current record only as a last resort; with
        // 16 physical COW records and post-commit retirement this should not occur normally.
        if(previous_rc==0) free_id=previous.id;
        else { fclose(f); return -3; }
    }
    const uint32_t generation=global_generation+1u;
    const long payload_off=(long)record_offset(free_id)+(long)SAV_RECORD_HEADER;
    bool ok=true;
    if(len) ok=fseek(f,payload_off,SEEK_SET)==0 && fwrite(data,1,len,f)==len;
    if(ok) ok=write_record_header(f,free_id,name,data,len,generation);
    if(ok) ok=write_global_header(f,generation);
    if(ok) ok=sync_file(f);
    // Retire every older generation for this DB only after the new generation
    // is durable. Duplicate records left by a power loss are harmless because
    // find_latest() selects the largest generation.
    if(ok) {
        for(int id=0;id<(int)FAT_RECORD_CAPACITY;id++) {
            if(id==free_id) continue;
            RecordInfo r{};
            if(read_record_header(f,id,&r) && !strcmp(r.name,basename_of(name))) clear_record_header(f,id);
        }
        ok=sync_file(f);
    }
    if(fclose(f)!=0) ok=false;
    if(ok) printf("[SAV] FAT write %s slot=%d len=%lu gen=%lu -> %s\n",basename_of(name),free_id,(unsigned long)len,(unsigned long)generation,g_fat_path);
    return ok?0:-3;
}
static int remove_record(const char *name) {
    FILE *f=open_rw(); if(!f) return -12;
    bool found=false, ok=true;
    for(int id=0;id<(int)FAT_RECORD_CAPACITY;id++) {
        RecordInfo r{};
        if(read_record_header(f,id,&r) && !strcmp(r.name,basename_of(name))) {
            found=true; if(!clear_record_header(f,id)) ok=false;
        }
    }
    uint32_t gen=0; global_header_valid(f,&gen);
    if(found && ok) ok=write_global_header(f,gen+1u) && sync_file(f);
    if(fclose(f)!=0) ok=false;
    if(!found) return -12;
    return ok?0:-3;
}
}

namespace ZenSav {
void InitFatFallback() {
    g_fat_available=false; g_fat_path[0]=0; g_native_available=false;
#if RV_CART_SAVE_PROFILE
    // melonDS retail-cart profile: prefer the emulated 128 KiB cartridge
    // EEPROM.  This produces the normal emulator-side .sav file automatically.
    if(native_probe_hw()) {
        g_native_available=true;
        unsigned char meta[NATIVE_META_SIZE];
        if(!native_read_meta(meta)) {
            native_meta_init(meta,0);
            if(!native_write_meta(meta)) {
                g_native_available=false;
                printf("[SAV!] EEPROM detected but metadata init failed\n");
            }
        }
        if(g_native_available) {
            printf("[SAV] melonDS/cart EEPROM backend ready: 128 KiB\n");
            return;
        }
    } else {
        printf("[SAV] 128 KiB cart EEPROM probe failed, trying FAT\n");
    }
#endif

    // DLDI/flashcart fallback.  With the #### profile this is the normal path.
    if(!fatInitDefault()) {
        printf("[SAV] FAT persistence unavailable\n");
        return;
    }
    static const char *prefixes[]={"sd:/","fat:/","fat:","./",""};
    for(unsigned i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++) {
        if(fat_probe_prefix(prefixes[i])) {
            snprintf(g_fat_path,sizeof(g_fat_path),"%sRAGNAROK.sav",prefixes[i]);
            g_fat_available=true;
            FILE *f=open_rw(); if(f) fclose(f);
            printf("[SAV] Ragnarok FAT backend ready: %s\n",g_fat_path);
            return;
        }
    }
    printf("[SAV] FAT mounted, no writable path\n");
}

void EnableNativeProbe() {
    // Retained for API compatibility.  Native EEPROM is selected once during
    // InitFatFallback() only in RV_CART_SAVE_PROFILE builds.
}
void Flush() {
    // Both backends commit synchronously.
}
bool IsPersistentName(const char *name) { return is_persistent_name(name); }
int RecordIdForName(const char *name) {
    const char *base=basename_of(name);
    const int slot=save_slot_index(base);
    if(slot>=0) return slot;
    if(ascii_ieq(base,"option") || ascii_ieq(base,"option.sav")) return 3;
    if(ascii_ieq(base,"list")) return 4;
    return -1;
}
bool Exists(const char *name,uint32_t *out_len) {
    if(out_len) *out_len=0;
    if(!is_persistent_name(name)) return false;
#if RV_CART_SAVE_PROFILE
    if(g_native_available && native_record_exists(name,out_len)) return true;
#endif
    return record_exists(name,out_len);
}
int Load(const char *name,uint8_t **out_data,uint32_t *out_len) {
    if(!is_persistent_name(name)) return -12;
#if RV_CART_SAVE_PROFILE
    if(g_native_available) {
        const int nrc=native_load_record(name,(unsigned char**)out_data,out_len);
        if(nrc==0) return 0;
        // A missing record is normal on first boot; only use FAT as a fallback
        // if it is actually available.
        if(!g_fat_available) return nrc;
    }
#endif
    const int rc=load_record(name,(unsigned char**)out_data,out_len);
    if(rc==0) printf("[SAV] load %s len=%lu via FAT\n",basename_of(name),(unsigned long)*out_len);
    return rc;
}
int Store(const char *name,const uint8_t *data,uint32_t len) {
    if(!is_persistent_name(name)) return -12;
#if RV_CART_SAVE_PROFILE
    if(g_native_available) {
        const int nrc=native_store_record(name,(const unsigned char*)data,len);
        if(nrc==0) return 0;
        printf("[SAV!] EEPROM store %s rc=%d\n",basename_of(name),nrc);
        if(!g_fat_available) return nrc;
    }
#endif
    return store_record(name,(const unsigned char*)data,len);
}
int Remove(const char *name) {
    if(!is_persistent_name(name)) return -12;
#if RV_CART_SAVE_PROFILE
    if(g_native_available) {
        const int nrc=native_remove_record(name);
        if(nrc==0 || !g_fat_available) return nrc;
    }
#endif
    return remove_record(name);
}
const char *BackendName() {
#if RV_CART_SAVE_PROFILE
    if(g_native_available) return "128K cart EEPROM (.sav)";
#endif
    return g_fat_available?"FAT RAGNAROK.sav":"RAM only";
}
}
