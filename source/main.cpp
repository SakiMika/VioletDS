#include <nds.h>
#include <filesystem.h>
#include <stdio.h>
#include <string.h>
#include "lgt_loader.h"
#include "wipi_shim.h"
#include "embedded_assets.h"

#pragma pack(push,1)
struct InitParam1 {
    uint8_t bootstrap[0x214];
    uint32_t ptr_init_struct;
};
struct InitParam2 {
    uint32_t fn_get_import_table;
    uint32_t fn_get_import_function;
    uint32_t fn_unk3;
    uint32_t fn_unk4;
};
struct InitStruct {
    uint32_t unknown;
    uint32_t fn_init;
    uint32_t init_name;
};
#pragma pack(pop)

extern "C" uint32_t resolver_table(uint32_t table) { return wipi_get_import_table(table); }
extern "C" uint32_t resolver_function(uint32_t table, uint32_t index) {
    return (uint32_t)(uintptr_t)wipi_get_import_function(table,index);
}

static PrintConsole bottom;
static LgtLoadedImage image;

// Real NDS/DSi hardware has separate D/I caches. Resolve the static 16-byte
// LGT import veneers up front so the guest does not depend on a self-modifying
// lazy resolver with stale cache lines.
static unsigned prepatch_import_region(uint8_t *base, uint32_t size, const char *name) {
    if(!base || size < 16) return 0;
    unsigned patched=0;
    for(uint32_t off=0; off+16<=size; off+=4) {
        uint32_t *w=(uint32_t*)(base+off);
        const uint32_t op0=w[0], op1=w[1], table=w[2], index=w[3];
        if(op0!=0xE52DE004u || (op1 & 0xFF000000u)!=0xEB000000u) continue;
        if(!(table==1u || table==0x1f8u || table==0x1fbu)) continue;
        if(index>=0x2000u) continue;
        void *fn=wipi_get_import_function(table,index);
        if(!fn) continue;
        w[0]=0xE59FC004u; // ldr ip,[pc,#4]
        w[1]=0xE12FFF1Cu; // bx ip
        w[2]=table;
        w[3]=(uint32_t)(uintptr_t)fn;
        patched++;
    }
    if(patched) {
        DC_FlushRange(base,size);
        IC_InvalidateRange(base,size);
    }
    printf("[HW] prepatch %s imports=%u\n",name?name:"?",patched);
    return patched;
}

static void prepatch_lgt_imports(LgtLoadedImage *img) {
    if(!img) return;
    unsigned n=0;
    n += prepatch_import_region(img->text,img->text_size,"text");
    n += prepatch_import_region(img->data,img->data_size,"data");
    DC_FlushAll();
    IC_InvalidateAll();
    printf("[HW] import veneers ready=%u\n",n);
}

static void fatal_wait(const char *msg) {
    wipi_set_log_visible(true);
    wipi_boot_stage(msg);
    printf("\n[FATAL] %s\nSTART = exit\n", msg);
    while(1){scanKeys();if(keysDown()&KEY_START)break;swiDelay(20000);}
}

static uint32_t read_cpsr(void) {
    uint32_t v=0;
    __asm__ volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

static void dump_irq_state(const char *tag) {
    printf("[IRQ] %s IME=%lu IE=%08lx IF=%08lx DISP=%04x CPSR=%08lx\n",
           tag?tag:"-",
           (unsigned long)REG_IME,
           (unsigned long)REG_IE,
           (unsigned long)REG_IF,
           (unsigned)REG_DISPSTAT,
           (unsigned long)read_cpsr());
}

// Notice-only Thumb immediates; leave shared font/camera coordinates intact.
// Validate the entire patch set before writing anything to the relocated guest.
static void patch_ragnarok_notice_layout(LgtLoadedImage *img) {
    struct Patch { uint32_t address; uint16_t before, after; };
    static const Patch patches[]={
        {0x1615Au,0x3964u,0x39A0u}, // panel: centerY - 100 -> centerY - 160 (y=0)
        {0x1617Au,0x3A5Au,0x3A9Cu}, // heading: y=70 -> y=4
        {0x1619Cu,0x3A41u,0x3A83u}, // body: y=95 -> y=29
        {0x161B8u,0x325Au,0x3210u}, // prompt: y=250 -> y=176
        {0x16148u,0x3250u,0x3206u}, // slime anchor: y=240 -> y=166
    };
    uint16_t *locations[sizeof(patches)/sizeof(patches[0])];
    for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++) {
        locations[i]=(uint16_t*)lgt_translate_ptr(img,patches[i].address);
        if(!locations[i] || *locations[i]!=patches[i].before) {
            printf("[RV!] notice layout opcode mismatch at %08lx; skipped\n",
                   (unsigned long)patches[i].address);
            return;
        }
    }
    for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++) {
        *locations[i]=patches[i].after;
        DC_FlushRange(locations[i],sizeof(uint16_t));
        IC_InvalidateRange(locations[i],sizeof(uint16_t));
    }
    printf("[RV] notice layout: heading=4 body=29 prompt=176 slime=166\n");
}

// Scope splash/loading placement to their original draw call sites.
static void patch_ragnarok_loader_layout(LgtLoadedImage *img) {
    struct Patch { uint32_t address; uint16_t before, after; };
    static const Patch patches[]={
        {0x16208u,0x681Au,0x2260u}, // OD ARTS: center Y=96
        {0x16280u,0x681Au,0x2260u}, // Gravity Mobile: center Y=96
        {0x017AEu,0x3614u,0x3604u}, // tall handset loading: y=180 -> y=164
        {0x017A6u,0x3632u,0x3622u}, // alternate handset loading: also lift 16px
    };
    uint16_t *locations[sizeof(patches)/sizeof(patches[0])];
    for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++) {
        locations[i]=(uint16_t*)lgt_translate_ptr(img,patches[i].address);
        if(!locations[i] || *locations[i]!=patches[i].before) {
            printf("[RV!] loader layout opcode mismatch at %08lx; skipped\n",
                   (unsigned long)patches[i].address);
            return;
        }
    }
    for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++) {
        *locations[i]=patches[i].after;
        DC_FlushRange(locations[i],sizeof(uint16_t));
        IC_InvalidateRange(locations[i],sizeof(uint16_t));
    }
    printf("[RV] splash center=96 loading lifted=16\n");
}

// Background uses two 240x160 image halves. Move both together without
// changing their native scale or the shared handset drawing function.
static void (*rv_background_draw)(uint32_t,int,int,int)=nullptr;
extern "C" void rv_draw_centered_background(uint32_t image,int x,int y,int anchor) {
    if(rv_background_draw) rv_background_draw(image,x,y-32,anchor);
}

static void patch_ragnarok_title_layout(LgtLoadedImage *img) {
    struct Patch { uint32_t address; uint16_t before, after; };
    static const Patch patches[]={
        {0x162CEu,0x2240u,0x2220u}, // title logo y=64 -> y=32
        {0x1637Eu,0x3C49u,0x24A4u}, // Press Any Key: y=164
        {0x162F8u,0x3C2Bu,0x24A4u}, // alternate height branch: same prompt Y
        {0x06060u,0x3D30u,0x3D80u}, // six menu rows: y=32,55,78,101,124,147
    };
    uint16_t *locations[sizeof(patches)/sizeof(patches[0])];
    uint32_t *background=(uint32_t*)lgt_translate_ptr(img,0x6040u);
    void *draw=lgt_translate_ptr(img,0x1484u);
    if(!background || !draw || *background!=((uint32_t)(uintptr_t)draw|1u)) {
        printf("[RV!] title background signature mismatch; skipped\n");
        return;
    }
    for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++) {
        locations[i]=(uint16_t*)lgt_translate_ptr(img,patches[i].address);
        if(!locations[i] || *locations[i]!=patches[i].before) {
            printf("[RV!] title layout opcode mismatch at %08lx; skipped\n",
                   (unsigned long)patches[i].address);
            return;
        }
    }
    rv_background_draw=(void (*)(uint32_t,int,int,int))((uintptr_t)draw|1u);
    *background=(uint32_t)(uintptr_t)&rv_draw_centered_background;
    DC_FlushRange(background,sizeof(uint32_t));
    for(unsigned i=0;i<sizeof(patches)/sizeof(patches[0]);i++) {
        *locations[i]=patches[i].after;
        DC_FlushRange(locations[i],sizeof(uint16_t));
        IC_InvalidateRange(locations[i],sizeof(uint16_t));
    }
    printf("[RV] background shift=-32 title=32 prompt=164 menu=32..147\n");
}

// Title sub-pages (save slots, settings, help, game inquiry, exit prompt and the
// network prompts) were authored for a 320px handset: every ribbon, frame, text
// row and button is "centerY(160) +/- n". Only the TOP LCD (192 rows) is visible,
// so their lower halves were clipped. Re-point the centerY literal pools used by
// exactly those pages to a private 96 (=192/2): the whole page lifts by 64px and
// stays centered, no individual offset is edited, and the backdrop (drawn by its
// own call through rv_draw_centered_background) does not move.
//   0x16B28 slot select (state 2), settings (3), help (4)
//   0x16F58 / 0x17218 / 0x17650 / 0x17A58 network prompts and sub-pages (5)
//   0x17A58 game inquiry (6)        0x17C00 exit prompt (7) and its notice (0x1f)
// This replaces the older per-immediate -64 patch of the slot page (those five
// Thumb immediates are left original; the alias yields identical positions and
// also lifts the slot page's overwrite/delete prompts, which were never moved).
static const uint32_t rv_menu_center_y=96;
static void patch_ragnarok_menu_pages(LgtLoadedImage *img) {
    static const uint32_t pools[]={0x16B28u,0x16F58u,0x17218u,0x17650u,0x17A58u,0x17C00u};
    struct Imm { uint32_t address; uint16_t original; };
    static const Imm slot_originals[]={
        {0x167E0u,0x3A50u}, // title ribbon  centerY-80
        {0x167FEu,0x3938u}, // row frames    centerY-56
        {0x1682Au,0x3936u}, // selection     centerY-54
        {0x16860u,0x3834u}, // slot details  centerY-52
        {0x1687Au,0x3A2Au}, // empty label   centerY-42
    };
    const void *center=lgt_translate_ptr(img,0x015003D0u);
    uint32_t *locations[sizeof(pools)/sizeof(pools[0])];
    if(!center) {
        printf("[RV!] menu pages: centerY global missing; skipped\n");
        return;
    }
    for(unsigned i=0;i<sizeof(pools)/sizeof(pools[0]);i++) {
        locations[i]=(uint32_t*)lgt_translate_ptr(img,pools[i]);
        if(!locations[i] || *locations[i]!=(uint32_t)(uintptr_t)center) {
            printf("[RV!] menu pages literal mismatch at %08lx; skipped\n",
                   (unsigned long)pools[i]);
            return;
        }
    }
    for(const Imm &m:slot_originals) {
        const uint16_t *op=(const uint16_t*)lgt_translate_ptr(img,m.address);
        if(!op || *op!=m.original) {
            printf("[RV!] menu pages: slot opcode %08lx is not original; skipped\n",
                   (unsigned long)m.address);
            return;
        }
    }
    for(unsigned i=0;i<sizeof(pools)/sizeof(pools[0]);i++) {
        *locations[i]=(uint32_t)(uintptr_t)&rv_menu_center_y;
        DC_FlushRange(locations[i],sizeof(uint32_t));
        IC_InvalidateRange(locations[i],sizeof(uint32_t));
    }
    printf("[RV] menu pages lifted=64 (centerY=96): slot/settings/help/inquiry/exit/network\n");
}

// Private read-only viewport values for gameplay, camera bounds and in-game UI.
// Keep the original 240x320 storage/phone globals and previously tuned menus.
static const uint32_t rv_gameplay_height=192;
static const uint32_t rv_gameplay_center_y=96;
static void patch_ragnarok_gameplay_viewport(LgtLoadedImage *img) {
    struct Alias { uint32_t address, original; const uint32_t *value; };
    static const Alias aliases[]={
        {0x01D60u,0x015003DCu,&rv_gameplay_height}, // initialize dialogue panel/text Y from DS height
        {0x03EA0u,0x015003DCu,&rv_gameplay_height},
        {0x04634u,0x015003DCu,&rv_gameplay_height},
        {0x047A4u,0x015003DCu,&rv_gameplay_height},
        {0x06258u,0x015003DCu,&rv_gameplay_height},
        {0x0B7BCu,0x015003DCu,&rv_gameplay_height},
        {0x1DCF4u,0x015003DCu,&rv_gameplay_height},
        {0x20AD8u,0x015003DCu,&rv_gameplay_height},
        {0x24698u,0x015003DCu,&rv_gameplay_height},
        {0x26448u,0x015003DCu,&rv_gameplay_height},
        {0x26614u,0x015003DCu,&rv_gameplay_height},
        {0x27978u,0x015003DCu,&rv_gameplay_height},
        {0x2820Cu,0x015003DCu,&rv_gameplay_height},
        {0x2F470u,0x015003DCu,&rv_gameplay_height},
        {0x2F7E8u,0x015003DCu,&rv_gameplay_height},
        {0x04628u,0x015003D0u,&rv_gameplay_center_y},
        {0x06254u,0x015003D0u,&rv_gameplay_center_y},
        {0x0630Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x06B88u,0x015003D0u,&rv_gameplay_center_y},
        {0x08A8Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x0AB24u,0x015003D0u,&rv_gameplay_center_y},
        {0x0ACE0u,0x015003D0u,&rv_gameplay_center_y},
        {0x0BA84u,0x015003D0u,&rv_gameplay_center_y},
        {0x0BCD0u,0x015003D0u,&rv_gameplay_center_y},
        {0x0CA30u,0x015003D0u,&rv_gameplay_center_y},
        {0x0CC1Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x0D1A8u,0x015003D0u,&rv_gameplay_center_y},
        {0x0D54Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x0DE84u,0x015003D0u,&rv_gameplay_center_y},
        {0x0E188u,0x015003D0u,&rv_gameplay_center_y},
        {0x0E50Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x0E744u,0x015003D0u,&rv_gameplay_center_y},
        {0x0EE18u,0x015003D0u,&rv_gameplay_center_y},
        {0x0F074u,0x015003D0u,&rv_gameplay_center_y},
        {0x0F3BCu,0x015003D0u,&rv_gameplay_center_y},
        {0x0F770u,0x015003D0u,&rv_gameplay_center_y},
        {0x0FB0Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x0FF18u,0x015003D0u,&rv_gameplay_center_y},
        {0x106ECu,0x015003D0u,&rv_gameplay_center_y},
        {0x10CDCu,0x015003D0u,&rv_gameplay_center_y},
        {0x110D8u,0x015003D0u,&rv_gameplay_center_y},
        {0x1142Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x11B08u,0x015003D0u,&rv_gameplay_center_y},
        {0x11E58u,0x015003D0u,&rv_gameplay_center_y},
        {0x1235Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x12810u,0x015003D0u,&rv_gameplay_center_y},
        {0x13348u,0x015003D0u,&rv_gameplay_center_y},
        {0x13778u,0x015003D0u,&rv_gameplay_center_y},
        {0x13B7Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x13F90u,0x015003D0u,&rv_gameplay_center_y},
        {0x14318u,0x015003D0u,&rv_gameplay_center_y},
        {0x14638u,0x015003D0u,&rv_gameplay_center_y},
        {0x1499Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x14F44u,0x015003D0u,&rv_gameplay_center_y},
        {0x15E20u,0x015003D0u,&rv_gameplay_center_y},
        {0x15F08u,0x015003D0u,&rv_gameplay_center_y},
        {0x15FFCu,0x015003D0u,&rv_gameplay_center_y},
        {0x18118u,0x015003D0u,&rv_gameplay_center_y},
        {0x1897Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x1A2E0u,0x015003D0u,&rv_gameplay_center_y},
        {0x1A6D4u,0x015003D0u,&rv_gameplay_center_y},
        {0x1AAE4u,0x015003D0u,&rv_gameplay_center_y},
        {0x1AE50u,0x015003D0u,&rv_gameplay_center_y},
        {0x1B23Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x1B678u,0x015003D0u,&rv_gameplay_center_y},
        {0x1BA60u,0x015003D0u,&rv_gameplay_center_y},
        {0x1BEECu,0x015003D0u,&rv_gameplay_center_y},
        {0x1C344u,0x015003D0u,&rv_gameplay_center_y},
        {0x1D9F8u,0x015003D0u,&rv_gameplay_center_y},
        {0x21898u,0x015003D0u,&rv_gameplay_center_y},
        {0x21938u,0x015003D0u,&rv_gameplay_center_y},
        {0x220E4u,0x015003D0u,&rv_gameplay_center_y},
        {0x22254u,0x015003D0u,&rv_gameplay_center_y},
        {0x25EF4u,0x015003D0u,&rv_gameplay_center_y},
        {0x26470u,0x015003D0u,&rv_gameplay_center_y},
        {0x26AD8u,0x015003D0u,&rv_gameplay_center_y},
        {0x26C6Cu,0x015003D0u,&rv_gameplay_center_y},
        {0x27028u,0x015003D0u,&rv_gameplay_center_y},
        {0x27474u,0x015003D0u,&rv_gameplay_center_y},
        {0x278FCu,0x015003D0u,&rv_gameplay_center_y},
        {0x27D84u,0x015003D0u,&rv_gameplay_center_y},
        {0x28214u,0x015003D0u,&rv_gameplay_center_y},
        {0x2E9CCu,0x015003D0u,&rv_gameplay_center_y},
        {0x16760u,0x015003D0u,&rv_gameplay_center_y},
    };
    // Validate every relocated literal before committing the complete set.
    for(const Alias &a:aliases) {
        uint32_t *location=(uint32_t*)lgt_translate_ptr(img,a.address);
        void *original=lgt_translate_ptr(img,a.original);
        if(!location || !original || *location!=(uint32_t)(uintptr_t)original) {
            printf("[RV!] gameplay viewport mismatch at %08lx; skipped\n",
                   (unsigned long)a.address);
            return;
        }
    }
    for(const Alias &a:aliases) {
        uint32_t *location=(uint32_t*)lgt_translate_ptr(img,a.address);
        *location=(uint32_t)(uintptr_t)a.value;
        DC_FlushRange(location,sizeof(uint32_t));
    }
    printf("[RV] gameplay viewport=240x192 centerY=96; story centered\n");
}

// Intro-only controls: keep the original SKIP and EZ:FAST sprite draw calls,
// but place both labels at y=164, safely above the 192px LCD edge.
static void patch_ragnarok_intro_controls(LgtLoadedImage *img) {
    const uint32_t sites[]={0x166ACu,0x166CCu};
    for(uint32_t site:sites) {
        const uint16_t *p=(const uint16_t*)lgt_translate_ptr(img,site);
        if(!p || *p!=0x3A14u) {
            printf("[RV!] intro control signature mismatch; skipped\n"); return;
        }
    }
    for(uint32_t site:sites) {
        uint16_t *p=(uint16_t*)lgt_translate_ptr(img,site);
        *p=0x22A4u; // movs r2,#164 (was subs r2,#20 from phone height)
        DC_FlushRange(p,2);
        IC_InvalidateRange(p,2);
    }
    printf("[RV] intro EZ:FAST / SKIP y=164\n");
}

// Observe the native map renderer without changing its draw or camera state.
static uint32_t (*rv_original_map_draw)(uint32_t)=nullptr;
extern "C" uint32_t rv_map_draw_for_sub(uint32_t self) {
    const uint32_t result=rv_original_map_draw(self);
    wipi_note_ragnarok_map(self);
    return result;
}
static void install_ragnarok_sub_map(LgtLoadedImage *img) {
    const uint32_t sites[]={0x27020u,0x274A0u,0x27910u};
    void *original=lgt_translate_ptr(img,0x3AFCu);
    if(!original) return;
    for(uint32_t site:sites) {
        uint32_t *p=(uint32_t*)lgt_translate_ptr(img,site);
        if(!p || *p!=((uint32_t)(uintptr_t)original|1u)) {
            printf("[RV!] SUB map signature mismatch\n"); return;
        }
    }
    // Key 3 opens UI state 101; 0x187CC calls this pure map renderer at
    // 0x187EC. Reuse that renderer without opening the menu or changing keys.
    const uint32_t *minimap_code=(const uint32_t*)lgt_translate_ptr(img,0x182C4u);
    const uint32_t *minimap_map=(const uint32_t*)lgt_translate_ptr(img,0x183B4u);
    void *map=lgt_translate_ptr(img,0x01503EACu);
    void *context=lgt_translate_ptr(img,0x01500640u);
    uint32_t *color=(uint32_t*)lgt_translate_ptr(img,0x0150CA9Cu);
    if(!minimap_code || *minimap_code!=0xB097B5F0u || !minimap_map ||
       *minimap_map!=(uint32_t)(uintptr_t)map || !context || !color) {
        printf("[RV!] key-3 minimap signature mismatch; skipped\n"); return;
    }
    wipi_set_ragnarok_native_minimap((uint32_t)(uintptr_t)minimap_code|1u,context,color);
    rv_original_map_draw=(uint32_t (*)(uint32_t))((uintptr_t)original|1u);
    for(uint32_t site:sites) {
        uint32_t *p=(uint32_t*)lgt_translate_ptr(img,site);
        *p=(uint32_t)(uintptr_t)&rv_map_draw_for_sub;
        DC_FlushRange(p,4);
    }
}

static uint32_t rv_original_quickbar=0;
extern "C" void rv_quickbar_for_sub(int,int,uint32_t flags) {
    wipi_draw_ragnarok_quickbar(rv_original_quickbar,flags);
}
static void install_ragnarok_sub_quickbar(LgtLoadedImage *img) {
    uint32_t *site=(uint32_t*)lgt_translate_ptr(img,0x2469Cu);
    void *original=lgt_translate_ptr(img,0x9778u);
    const uint8_t coords[]={0,0,21,0,63,0,84,0};
    const void *guest_coords=lgt_translate_ptr(img,0x0140028Du);
    if(!site || !original || !guest_coords ||
       *site!=((uint32_t)(uintptr_t)original|1u) ||
       memcmp(guest_coords,coords,sizeof(coords))!=0) {
        printf("[RV!] quickbar signature mismatch; skipped\n"); return;
    }
    rv_original_quickbar=(uint32_t)(uintptr_t)original|1u;
    *site=(uint32_t)(uintptr_t)&rv_quickbar_for_sub;
    DC_FlushRange(site,4);
    printf("[RV] native quickbar -> SUB y172..191; touch 1/3/7/9\n");
}

int main(void) {
    powerOn(POWER_ALL_2D);
    soundEnable();
    soundSetMixerVolume(127);
    wipi_init_video(&bottom);
    consoleSelect(&bottom);
    consoleClear();
    printf("Ragnarok Violet WIPI -> NDS\n");
    printf("Guest: 240x320 RGB565\n");
    printf("TOP=y0..159 bottom-aligned, BOTTOM=y160..319 top-aligned\n");
    printf("Keys: D-pad move, A=EZ B=CLR X=0 Y=* R=#\n");
    printf("L+A/B/X/Y=1/3/7/9, START=Call, SELECT=unused\n");
    printf("Save: 128K cartridge EEPROM (.sav)\n\n");
    // Diagnostic build: keep the SUB console visible throughout native boot.
    // A guest crash inside entry/init/start cannot be recovered by the main
    // event loop, so hiding the log here makes the failure unobservable.
    // Show the first stage before touching the VRAM bank swap for LOG.
    // If that swap itself fails, the TOP still identifies the failure.
    wipi_boot_stage("01 VIDEO READY");
    wipi_set_log_visible(true);
    wipi_boot_stage("02 OPEN NITROFS");

    if(!nitroFSInit(NULL)) { fatal_wait("NitroFS init failed"); return 1; }
    uint32_t boot_binary_size=0;
    const bool binary_before_save=embedded_asset_size("binary.mod",&boot_binary_size);
    printf("[FS] binary before save=%s size=%lu mode=%s\n",
           binary_before_save?"OK":"FAIL",(unsigned long)boot_binary_size,
           isDSiMode()?"DSi":"DS");
    if(!binary_before_save) { fatal_wait(embedded_asset_last_error()); return 1; }
    const uint16_t rom_control_before_save=REG_AUXSPICNT;
    wipi_init_storage();
    printf("[FS] card control %04x -> %04x\n",
           (unsigned)rom_control_before_save,(unsigned)REG_AUXSPICNT);

    printf("[ROM] NitroFS payload: %lu bytes / %lu files\n",
           (unsigned long)embedded_asset_pack_size(),
           (unsigned long)embedded_asset_count());
    EmbeddedAssetView binary{};
    if(!embedded_asset_find("binary.mod",&binary)) { fatal_wait(embedded_asset_last_error()); return 1; }
    printf("[ROM] binary.mod %lu bytes\n",(unsigned long)binary.size);
    printf("[VID] split portrait framebuffer, no scaling\n");
    printf("[SPD] guest clock = 1.00x\n");

    wipi_boot_stage("03 LOAD ELF");
    if(!lgt_load_binary_memory(binary.data, binary.size, &image)) {
        fatal_wait("binary.mod load failed"); return 2;
    }
    wipi_set_guest_text_mapping(image.text_old,image.text,image.text_size);
    embedded_asset_release_cache();

    // Do not install any Zenonia-specific address/signature hooks here.
    wipi_boot_stage("04 PATCH IMPORT");
    prepatch_lgt_imports(&image);
    patch_ragnarok_notice_layout(&image);
    patch_ragnarok_loader_layout(&image);
    patch_ragnarok_title_layout(&image);
    patch_ragnarok_menu_pages(&image);
    patch_ragnarok_gameplay_viewport(&image);
    patch_ragnarok_intro_controls(&image);
    install_ragnarok_sub_map(&image);
    install_ragnarok_sub_quickbar(&image);

    InitParam1 p1{};
    InitParam2 p2{};
    p2.fn_get_import_table=(uint32_t)(uintptr_t)&resolver_table;
    p2.fn_get_import_function=(uint32_t)(uintptr_t)&resolver_function;

    typedef void (*EntryFn)(InitParam1*, InitParam2*, uint32_t);
    EntryFn entry=(EntryFn)(image.entry_new | 1u);
    DC_FlushAll(); IC_InvalidateAll();
    printf("[BOOT] calling Ragnarok ELF entry...\n");
    wipi_boot_stage("05 CALL ENTRY");
    entry(&p1,&p2,0);
    printf("[BOOT] entry returned, desc=%08lx\n",(unsigned long)p1.ptr_init_struct);

    InitStruct *desc=(InitStruct*)lgt_translate_ptr(&image,p1.ptr_init_struct);
    if(!desc) desc=(InitStruct*)(uintptr_t)p1.ptr_init_struct;
    if(!desc || !desc->fn_init) { fatal_wait("invalid init descriptor"); return 3; }

    const uint32_t init_old=desc->fn_init;
    void *init_new=lgt_translate_ptr(&image,init_old);
    if(!init_new) init_new=(void*)(uintptr_t)init_old;
    printf("[BOOT] init %08lx -> %p\n",(unsigned long)init_old,init_new);
    typedef void (*InitFn)(void);
    InitFn init=(InitFn)((uintptr_t)init_new | (init_old&1u));
    DC_FlushAll(); IC_InvalidateAll();
    wipi_boot_stage("06 CALL INIT");
    init();
    printf("[BOOT] initializer returned\n");
    dump_irq_state("after init");

    wipi_boot_stage("07 CHECK CLET");
    const WipiCletState *cs=wipi_clet_state();
    printf("[STATE] registered=%s\n",cs->registered?"YES":"NO");
    if(!cs->registered) { fatal_wait("CletRegister missing"); return 4; }

    // CletRegister may return callbacks in original ELF virtual address space.
    WipiCletState *csrw=const_cast<WipiCletState*>(cs);
    for(int i=0;i<6;i++) {
        const uint32_t raw=csrw->callbacks[i];
        const uint32_t thumb=raw&1u;
        void *mapped=raw ? lgt_translate_ptr(&image,raw&~1u) : nullptr;
        if(mapped) csrw->callbacks[i]=(uint32_t)(uintptr_t)mapped|thumb;
        const uint32_t fixed=csrw->callbacks[i];
        const bool ram=((fixed&~1u)>=0x02000000u && (fixed&~1u)<0x04000000u);
        printf("[CB%d] %08lx -> %08lx %s%s\n",i,
               (unsigned long)raw,(unsigned long)fixed,
               mapped?"REBASE ":"DIRECT ",ram?"RAM":"BAD");
    }

    // Ragnarok Violet performs a one-time handset reboot gate during startup.
    // Old guest 0x000027D8 is Thumb `beq 0x27E2`: when the persisted
    // restart-complete flag at 0x015003A0 is zero it enters UI state 0x16 and
    // asks the phone firmware to reboot.  A DS homebrew process cannot reboot
    // an LGT handset, so take the exact post-reboot fall-through path instead.
    // Validate the original opcode before touching guest code so a different
    // build of binary.mod cannot be silently patched at the wrong address.
    {
        volatile uint16_t *restart_gate=(volatile uint16_t*)lgt_translate_ptr(&image,0x000027D8u);
        if(restart_gate) {
            const uint16_t old=*restart_gate;
            if(old==0xD003u) { // beq +3 -> 0x27E2
                *restart_gate=0x46C0u; // Thumb NOP: fall through to 0x27DA
                DC_FlushRange((void*)restart_gate,sizeof(uint16_t));
                IC_InvalidateAll();
                printf("[RV] restart gate bypass 27D8: D003 -> 46C0\n");
            } else {
                printf("[RV!] restart gate opcode=%04x, bypass skipped\n",(unsigned)old);
            }
        } else {
            printf("[RV!] restart gate 27D8 not mapped, bypass skipped\n");
        }
    }

    // Ragnarok Violet-specific startup EZ bridge.  Kept as a diagnostic
    // fallback only; with the restart gate bypassed state 0x16 should not be
    // entered at all.  The binary's UI key handler is Thumb function 0xB110.
    volatile uint32_t *rv_game_state=(volatile uint32_t*)lgt_translate_ptr(&image,0x015009C0u);
    void *rv_ui_key_handler=lgt_translate_ptr(&image,0x0000B110u);
    wipi_set_ragnarok_ez_bridge(rv_game_state,rv_ui_key_handler);

    dump_irq_state("before start");
    wipi_boot_stage("08 START CLET");
    if(!wipi_start_clet()) { fatal_wait("startClet unavailable"); return 5; }

    printf("[RUN] Ragnarok Violet first-pass running\n");
    printf("[RUN] X+Y+START = quit diagnostic build\n");

    wipi_boot_stage("09 PAINT CLET");
    wipi_paint_clet();
    wipi_boot_stage("10 GAME LOOP");
    wipi_present();
    wipi_debug_dump("after paint");
    // Boot diagnostics served their purpose once the game loop is alive.  Show
    // the lower half of the 240x320 guest by default; tap LOG to reopen it.
    wipi_set_log_visible(false);

    while(1) {
        scanKeys();
        const uint32_t down=keysDown();
        const uint32_t up=keysUp();
        const uint32_t held=keysHeld();
        if((down&KEY_START) && (held&KEY_X) && (held&KEY_Y)) break;
        // Touch targets the native quickbar and the settings/log controls.
        if(down & KEY_TOUCH) {
            touchPosition touch{};
            touchRead(&touch);
            wipi_handle_touch(down, touch.px, touch.py);
        }
        wipi_dispatch_keys(down,up);
        wipi_poll();
    }
    wipi_flush_storage();
    return 0;
}
