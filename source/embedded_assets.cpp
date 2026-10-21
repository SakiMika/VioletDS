#include "embedded_assets.h"
#include <calico/nds/nitrorom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Override Calico's weak default DMA channel. CPU polling avoids zero-filled
// cartridge transfers under DS direct boot and does not share graphics DMA.
extern "C" {
extern const int8_t g_nitroromCardDmaChannel=-1;
}

namespace {
static uint8_t *g_cache=nullptr;
static size_t g_cache_cap=0;
static char g_error[64]="ASSET OK";

static uint8_t *g_fnt=nullptr;
static uint32_t g_fnt_size=0;
static uint16_t get16(const uint8_t *p) { return p[0]|((uint16_t)p[1]<<8); }
static uint32_t get32(const uint8_t *p) {
    return get16(p)|((uint32_t)get16(p+2)<<16);
}
static bool cache_fnt(NitroRom *rom) {
    if(g_fnt) return true;
    // The application's ROM may differ from the physical Slot-1 cartridge.
    // In DS direct boot only the application header is guaranteed to describe it.
    const EnvNdsHeader *header=g_envAppNdsHeader;
    if(header->fnt_rom_offset!=rom->fnt_offset) {
        strcpy(g_error,"FNT APP OFFSET MISMATCH"); return false;
    }
    const uint32_t bytes=header->fnt_size;
    if(bytes<8 || bytes>256*1024u) { strcpy(g_error,"FNT HEADER INVALID"); return false; }
    uint8_t *table=(uint8_t*)malloc(bytes);
    if(!table) { strcpy(g_error,"FNT ALLOC FAILED"); return false; }
    if(!nitroromRead(rom,rom->fnt_offset,table,bytes)) {
        free(table); strcpy(g_error,"FNT ROM READ FAILED"); return false;
    }
    const uint32_t dirs=get16(table+6);
    if(!dirs || dirs!=rom->num_dirs || dirs*8u>bytes || get32(table)<dirs*8u || get32(table)>=bytes) {
        snprintf(g_error,sizeof(g_error),"FNT BAD %08lX %lu %u",(unsigned long)get32(table),(unsigned long)dirs,(unsigned)rom->num_dirs);
        free(table); return false;
    }
    g_fnt=table; g_fnt_size=bytes;
    return true;
}
static int resolve_cached_path(const char *name) {
    uint32_t dir=0;
    while(*name) {
        const char *slash=strchr(name,'/');
        const size_t len=slash?(size_t)(slash-name):strlen(name);
        if(!len || dir*8u+8u>g_fnt_size) return -1;
        uint32_t pos=get32(g_fnt+dir*8u);
        uint16_t file=get16(g_fnt+dir*8u+4);
        bool found=false;
        while(pos<g_fnt_size) {
            const uint8_t type=g_fnt[pos++];
            if(!type) break;
            const uint32_t n=type&127u;
            const bool folder=(type&128u)!=0;
            if(!n || pos+n+(folder?2u:0u)>g_fnt_size) return -1;
            if(n==len && !memcmp(g_fnt+pos,name,len)) {
                if(!slash) return folder?-1:file;
                if(!folder) return -1;
                const uint16_t child=get16(g_fnt+pos+n);
                if(child<NITROROM_ROOT_DIR) return -1;
                dir=child-NITROROM_ROOT_DIR; name=slash+1; found=true; break;
            }
            pos+=n+(folder?2u:0u);
            if(!folder) ++file;
        }
        if(!found) return -1;
    }
    return -1;
}

// Read through the mounted ROM API: no stdio buffering or seek translation.
static int resolve_asset(const char *name,NitroRom **rom,uint32_t *size) {
    if(!name) { strcpy(g_error,"ASSET BAD NAME"); return -1; }
    while(*name=='/' || *name=='\\') ++name;
    *rom=nitroromGetSelf();
    if(!*rom) { strcpy(g_error,"ASSET ROM ACCESS FAILED"); return -1; }
    if(!cache_fnt(*rom)) return -1;
    const int id=resolve_cached_path(name);
    if(id<0 || id>=(*rom)->num_files) {
        snprintf(g_error,sizeof(g_error),"ASSET PATH FAILED %d",id);
        return -1;
    }
    *size=nitroromGetFileSize(*rom,(u16)id);
    return id;
}
static bool read_asset(NitroRom *rom,int id,void *dst,uint32_t size) {
    // Small reads bound scratch-buffer use on the 4 MiB DS heap.
    for(uint32_t off=0;off<size;) {
        const uint32_t chunk=size-off>4096u?4096u:size-off;
        if(!nitroromReadFile(rom,(u16)id,off,(uint8_t*)dst+off,chunk)) {
            snprintf(g_error,sizeof(g_error),"ASSET ROM READ %lu",(unsigned long)off);
            return false;
        }
        off+=chunk;
    }
    strcpy(g_error,"ASSET OK");
    return true;
}
}
const char *embedded_asset_last_error(void) { return g_error; }
bool embedded_asset_size(const char *name,uint32_t *out_size) {
    if(out_size) *out_size=0;
    NitroRom *rom=nullptr; uint32_t size=0;
    if(resolve_asset(name,&rom,&size)<0) return false;
    if(out_size) *out_size=size;
    return true;
}
int embedded_asset_read(const char *name,void *dst,uint32_t cap) {
    if(!dst) return -9;
    NitroRom *rom=nullptr; uint32_t size=0;
    const int id=resolve_asset(name,&rom,&size);
    if(id<0) return -12;
    if(size>cap) return -18;
    return read_asset(rom,id,dst,size)?0:-12;
}
bool embedded_asset_find(const char *name,EmbeddedAssetView *out) {
    if(out) { out->data=nullptr; out->size=0; }
    NitroRom *rom=nullptr; uint32_t size=0;
    const int id=resolve_asset(name,&rom,&size);
    if(id<0) return false;
    if(size>g_cache_cap) {
        const size_t cap=((size_t)size+31u)&~31u;
        uint8_t *p=(uint8_t*)realloc(g_cache,cap?cap:32);
        if(!p) {
            snprintf(g_error,sizeof(g_error),"ASSET ALLOC %lu FAILED",(unsigned long)cap);
            return false;
        }
        g_cache=p; g_cache_cap=cap;
    }
    if(!read_asset(rom,id,g_cache,size)) return false;
    if(out) { out->data=g_cache; out->size=size; }
    return true;
}
void embedded_asset_release_cache(void) {
    free(g_cache); g_cache=nullptr; g_cache_cap=0;
}
uint32_t embedded_asset_count(void) { return 1279u; }
uint32_t embedded_asset_pack_size(void) { return 3880326u; }
