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
    wipi_boot_stage("ERROR BOOT STOPPED");
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
    printf("L+A/B/X/Y=1/3/7/9, START=Call, SELECT=SUB toggle\n");
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
    wipi_init_storage();

    printf("[ROM] NitroFS payload: %lu bytes / %lu files\n",
           (unsigned long)embedded_asset_pack_size(),
           (unsigned long)embedded_asset_count());
    EmbeddedAssetView binary{};
    if(!embedded_asset_find("binary.mod",&binary)) { fatal_wait("NitroFS binary.mod missing"); return 1; }
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
        // Touch is reserved for DS-side LOG/CLOSE controls. Never forward it
        // as a WIPI game key; the lower LCD continues to show guest y=160..319
        // whenever the diagnostic console is closed.
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
