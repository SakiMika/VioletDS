#include "zen_boot_profile.h"
#include "wipi_shim.h"
#include "embedded_assets.h"
#include "zen_sav_backend.h"
#include "ragnarok_layout_policy.h"
#include "rv_png_cache_table.h"
#include "zen_map_ui.h"
#include "zen_render_fast.h"
#include "zen_ds_policy.h"

#ifndef ZEN_CONSERVATIVE_CALLBACK_CACHE
#define ZEN_CONSERVATIVE_CALLBACK_CACHE 0
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>
#include <fat.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

namespace {
constexpr int PHONE_W=RagnarokLayout::Width;
constexpr int PHONE_H=RagnarokLayout::Height;
constexpr int DS_W=256;
constexpr int DS_H=192;
constexpr int TOP_SRC_Y=32;
constexpr int UI_HUD_SRC_X=0;
constexpr int UI_HUD_SRC_Y=0;
constexpr int UI_HUD_W=72;
constexpr int UI_HUD_H=48;
constexpr int UI_DIALOG_INFO_SRC_Y=96;
constexpr int UI_DIALOG_CUTSCENE_SRC_Y=128;
constexpr int UI_DIALOG_CAPTURE_H=192; // capture a full lower-screen page so avatar + text fit together
constexpr int UI_QUICK_SRC_X=48;
constexpr int UI_QUICK_SRC_Y=278;
constexpr int UI_QUICK_W=144;
constexpr int UI_QUICK_H=42;
constexpr int UI_QUICK_DST_X=56;
constexpr int UI_QUICK_DST_Y=146;
constexpr int LOG_BTN_X=236;
constexpr int LOG_BTN_Y=4;
constexpr int LOG_BTN_W=16;
constexpr int LOG_BTN_H=16;
// Ragnarok first-pass: fit handset width to TOP; retain native vertical pixels.
constexpr uint8_t SUB_BG_COL=0;
constexpr uint8_t SUB_PANEL_COL=18;
constexpr uint8_t SUB_FRAME_COL=15;
// Pace guest callbacks in real time, including zero-delay timer rearming.
constexpr uint32_t GUEST_TIME_NUM=1;
constexpr uint32_t GUEST_TIME_DEN=1;
constexpr int TIMER_SLOTS=32;
constexpr uint64_t GUEST_TIMER_MIN_INTERVAL_MS=50; // at most 20 updates/sec per timer
constexpr int DB_SLOTS=24;
constexpr int UNKNOWN_THUNK_SLOTS=192;
constexpr int KNOWN_TRACE_THUNK_SLOTS=160;

// WIPI Runner lower-screen settings UI, adapted from the supplied NDS runner.
// The old minimap/map tab is intentionally NOT included: LOG opens only
// SINGLE, COMBO and LOG tabs.
constexpr int UI_TAB_SINGLE=0;
constexpr int UI_TAB_COMBO=1;
constexpr int UI_TAB_LOG=2;
constexpr int UI_TARGET_COUNT=11;
// 16px header + eleven 16px touch rows exactly fill the 192px LCD.
constexpr int UI_TARGET_TOP=16;
constexpr int UI_TARGET_ROW_H=16;
static_assert(UI_TARGET_TOP+UI_TARGET_COUNT*UI_TARGET_ROW_H==192,"settings rows must fit SUB");
constexpr int UI_LOG_CAP=24;
constexpr int UI_LOG_LINES=18;

struct RunnerTargetDef { const char *name; int wipi_key; };
static const RunnerTargetDef g_runner_targets[UI_TARGET_COUNT] = {
    // Preserve the original zenonia2-dsi-main/README.md key mapping, not the
    // provisional first-pass mapping. LGT's EZ/confirm action is the WIPI
    // FIRE key (-5); CLR=-16 and Call=-10 are handset key codes (NOT the
    // numeric characters '5'/'0' or WIPI softkeys).
    // SELECT is reserved for lower-screen visibility while Ragnarok is split.
    {"EZ", -5}, {"CLR", -16},
    {"1", '1'}, {"3", '3'}, {"5", '5'}, {"7", '7'}, {"9", '9'},
    {"*", '*'}, {"0", '0'}, {"#", '#'}, {"Call", -10}
};
static const uint32_t g_runner_physical_keys[] = {
    KEY_L, KEY_R, KEY_A, KEY_B, KEY_X, KEY_Y,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_START, KEY_SELECT
};
static const char *g_runner_physical_names[] = {
    "L", "R", "A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "START", "SELECT"
};
constexpr int UI_PHYSICAL_COUNT=(int)(sizeof(g_runner_physical_keys)/sizeof(g_runner_physical_keys[0]));

#pragma pack(push,4)
struct WipiFramebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t bpl;
    uint32_t bpp;
    uint32_t buf;
};
struct WipiDisplayInfo {
    uint32_t bpp;
    uint32_t depth;
    uint32_t width;
    uint32_t height;
    uint32_t bpl;
    uint32_t color_type;
    uint32_t red_mask;
    uint32_t blue_mask;
    uint32_t green_mask;
};
// LGT WIPI graphics context ABI is 56 bytes.  Zenonia 2 passes this record
// between its native wrappers and the WIPI service table, so the field offsets
// must match the handset ABI (not the older compact Inotia approximation).
struct WipiGraphicsContext {
    int32_t clip_x1;
    int32_t clip_y1;
    int32_t clip_x2;
    int32_t clip_y2;
    uint32_t foreground;
    uint32_t background;
    uint32_t alpha;
    uint32_t pixel_op;
    uint32_t pixel_param;
    uint32_t font;
    uint32_t style;
    uint32_t xor_enabled;
    int32_t offset_x;
    int32_t offset_y;
};
static_assert(sizeof(WipiGraphicsContext)==56, "LGT graphics context ABI must be 56 bytes");
// LGT public graphics handles. These two layouts are part of the native ABI:
// image = {backing framebuffer, public framebuffer wrapper}; framebuffer wrapper
// = {owned_image, graphics view, backing framebuffer, screen kind}.
struct LgtFramebuffer {
    uint32_t owned_image;
    uint32_t ptr_graphics;
    uint32_t ptr_image;
    uint32_t screen_kind;
};
struct LgtImage {
    uint32_t ptr_image;
    uint32_t ptr_framebuffer;
};
struct LgtGraphicsView {
    uint32_t ptr_backing;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(LgtFramebuffer)==16, "LGT framebuffer ABI must be 16 bytes");
static_assert(sizeof(LgtImage)==8, "LGT image ABI must be 8 bytes");
static_assert(sizeof(LgtGraphicsView)==20, "LGT graphics view ABI must be 20 bytes");
#pragma pack(pop)

struct TimerSlot {
    void *timer;
    void (*callback)(void*,void*);
    void *param;
    uint64_t due_ms;
    uint64_t next_fire_real_ms;
    bool armed;
};

constexpr uint32_t RV_IMAGE_MAGIC=0x31495652u; // "RVI1"
struct RvImageAllocation {
    LgtImage image;               // returned guest handle; must stay first
    LgtFramebuffer framebuffer;   // returned by GetImageFramebuffer
    LgtGraphicsView view;         // native LGT code can inspect this directly
    WipiFramebuffer backing;      // RGB565 pixels
    WipiFramebuffer mask;         // optional 8-bit alpha plane
    uint32_t magic;
};
constexpr uint32_t RV_FB_MAGIC=0x31424652u; // "RFB1"
struct RvFramebufferAllocation {
    LgtFramebuffer framebuffer;   // returned guest handle; must stay first
    LgtGraphicsView view;
    WipiFramebuffer backing;
    uint32_t magic;
};

static uint16_t *g_phone_fb=nullptr;
static uint16_t *g_ds_fb=nullptr;
static uint16_t *g_sub_fb=nullptr;
static uint16_t *g_sub_comp=nullptr;
static bool g_native_hud_active=false;
static int g_main_bg=-1;
static int g_sub_bg=-1;
static int g_log_bg=-1;
static PrintConsole *g_log_console=nullptr;
static bool g_log_visible=false;
// Ragnarok Violet uses a native UI key processor for handset-level dialogs.
// The normal CLET event path reaches it only in selected input modes.  Keep a
// surgical bridge for the startup restart prompt (game state 0x16) so DS A is
// always the phone's EZ/FIRE key without changing gameplay mappings.
static volatile uint32_t *g_rv_game_state=nullptr;
typedef void (*RvUiKeyHandler)(int);
static RvUiKeyHandler g_rv_ui_key_handler=nullptr;
static bool g_rv_direct_ez_held=false;
static uint64_t g_log_last_draw_ms=0;
// SELECT: hide/show the SUB view, retaining the original source's dedicated
// display-toggle key. The minimap itself is disabled in the split-screen port.
static bool g_bottom_half_visible=true;
static uint32_t g_rv_map_self=0;
static uint32_t g_touch_logical=0;
static const int RV_QUICK_TARGETS[6]={2,3,5,6,8,9};
static bool g_log_button_drawn=false;
static uint16_t *g_dialogue_snapshot=nullptr;
static bool g_dialogue_snapshot_valid=false;
static int g_dialogue_hold_frames=0;
static bool g_dialogue_last_visible=false;
static int g_dialogue_capture_y=UI_DIALOG_CUTSCENE_SRC_Y;
static WipiFramebuffer g_screen_fb{};
static LgtGraphicsView g_screen_view{};
static LgtFramebuffer g_screen_wrapper{};
static uint16_t *g_minimap_fb=nullptr;
static bool g_minimap_ready=false,g_minimap_bottom=false,g_minimap_capture=false,g_minimap_valid=false;
static int g_minimap_w=0,g_minimap_h=0;
static ZenDS::MinimapCache g_minimap_cache;
static ZenDS::PanelCache g_minimap_panel;
static ZenDS::RepaintGate g_repaint_gate;
static WipiFramebuffer g_minimap_alias{};
static WipiCletState g_clet{};
static TimerSlot g_timers[TIMER_SLOTS]{};
struct UnknownThunkEntry {
    uint32_t table;
    uint32_t index;
    uint32_t *code;
    bool called;
};
static UnknownThunkEntry g_unknown[UNKNOWN_THUNK_SLOTS]{};
static uint32_t *g_thunk_pool=nullptr;
static int g_thunk_count=0;

struct KnownTraceThunkEntry {
    uint32_t table;
    uint32_t index;
    void *target;
    uint32_t *code;
};
static KnownTraceThunkEntry g_known_trace[KNOWN_TRACE_THUNK_SLOTS]{};
static uint32_t *g_known_trace_pool=nullptr;
static int g_known_trace_count=0;
static uint32_t g_known_trace_budget=0;

// Runtime<->ELF mapping is supplied by main.cpp after loading binary.mod.
// It lets a photographed callback PC be mapped back to the exact address in
// the original ELF for deterministic disassembly.
static uint32_t g_guest_text_old=0;
static uintptr_t g_guest_text_new=0;
static uint32_t g_guest_text_size=0;

static uint32_t g_fallback_stub_count=0;
static uint64_t g_timer_clock_ms=0;
// v009: use Calico's 64-bit monotonic tick counter instead of accumulating
// cpuGetTiming() deltas. This keeps WIPI timers independent of VBlank/guest IRQ state.
static uint64_t g_tick_base=0;
static bool g_clock_started=false;
static uint64_t g_timer_wait_next_ms=0;
static uint64_t g_loop_report_next_ms=0;
static uint32_t g_loop_count=0;
static bool g_trace_next_timer=false;
static bool g_trace_repaint_once=false;
static uint32_t g_db_trace_budget=0;
static uint32_t g_unknown_trace_budget=0;
static uint32_t g_mem_trace_budget=0;
static uint32_t g_timer_trace_budget=0;
static uint32_t g_gfx_trace_budget=0;
static bool g_repaint_pending=false;
static bool g_present_pending=false;
static bool g_sub_static_valid=false;
static bool g_in_paint=false;

static uint32_t g_ui_single_map[UI_TARGET_COUNT]{};
static uint32_t g_ui_combo_map1[UI_TARGET_COUNT]{};
static uint32_t g_ui_combo_map2[UI_TARGET_COUNT]{};
static uint32_t g_ui_prev_logical=0;
static int g_ui_tab=UI_TAB_LOG;
static int g_ui_capture_single=-1;
static int g_ui_capture_combo=-1;
static int g_ui_selected_single=0;
static int g_ui_selected_combo=0;
static bool g_ui_dirty=true;
static char g_ui_log_lines[UI_LOG_CAP][32]{};
static int g_ui_log_count=0;
static int g_ui_log_head=0;

// v020: persistent, low-noise audio diagnostics shown in the LOG header.
static uint32_t g_audio_create_count=0;
static uint32_t g_audio_prepare_count=0;
static uint32_t g_audio_play_count=0;
static uint32_t g_audio_ok_count=0;
static uint32_t g_audio_fail_count=0;
static uint32_t g_audio_bytes_count=0;
static uint32_t g_audio_stop_count=0;
static uint32_t g_audio_free_count=0;
static uint32_t g_audio_last_peak=0;
static uint32_t g_audio_last_rate=0;
static uint32_t g_audio_last_samples=0;
static int32_t g_audio_last_channel=-1;
static int32_t g_audio_last_requested_volume=100;
static uint8_t g_audio_last_kind=0;
static uint32_t g_audio_end_count=0;
struct DummyMediaClip;
constexpr int MEDIA_CLIP_SLOTS=16;
static DummyMediaClip *g_media_clips[MEDIA_CLIP_SLOTS]{};
static char g_audio_last_asset[32]{};
static int g_media_profile_volume=100;
static int g_media_device_enabled=1;
static uint32_t g_audio_start_count=0;
// v025 Zenonia-specific BGM recovery. The original game sometimes opens a
// 1xx.mmf BGM resource but never reaches the Play call on this partial WIPI
// implementation. Track the last genuine BGM asset and start it as a loop only
// if the game does not do so itself shortly afterwards.
static char g_audio_last_bgm_asset[32]{};
static bool g_audio_bgm_pending=false;
static uint64_t g_audio_bgm_seen_ms=0;
static bool g_audio_game_bgm_started=false;
static DummyMediaClip *g_audio_fallback_bgm=nullptr;
static DummyMediaClip *g_audio_active_bgm=nullptr;
static uint32_t g_audio_fallback_bgm_count=0;
// v028: exact sound-request SFX recovery. Zenonia 2 formats sound/%03d.mmf at the point
// an effect is requested. If the native media manager does not reach Player.play
// shortly afterwards, play that same MMF once on a free DS channel.
static char g_audio_last_sfx_asset[32]{};
static bool g_audio_sfx_pending=false;
static uint64_t g_audio_sfx_seen_ms=0;
static uint64_t g_audio_clet_started_ms=0;
static uint32_t g_audio_sfx_play_snapshot=0;
static uint32_t g_audio_fallback_sfx_count=0;
static uint64_t g_audio_last_sfx_fire_ms=0;

static uint64_t monotonic_ms(void);
static uint64_t real_monotonic_ms(void);

static int audio_asset_numeric_id(const char *name) {
    if(!name) return -1;
    const char *base=strrchr(name,'/');
    if(!base) base=strrchr(name,'\\');
    base=base?base+1:name;
    if(base[0]<'0' || base[0]>'9') return -1;
    int id=0, digits=0;
    while(base[digits]>='0' && base[digits]<='9' && digits<4) {
        id=id*10+(base[digits]-'0');
        digits++;
    }
    if(digits!=3) return -1;
    return id;
}

static bool audio_asset_is_bgm(const char *name) {
    const int id=audio_asset_numeric_id(name);
    return id>=100 && id<=199;
}

static bool audio_asset_is_sfx(const char *name) {
    const int id=audio_asset_numeric_id(name);
    return id>=0 && id<100;
}

static void audio_note_asset(const char *name) {
    if(!name) return;
    const char *ext=strstr(name,".mmf");
    if(!ext) ext=strstr(name,".MMF");
    if(!ext) return;
    strncpy(g_audio_last_asset,name,sizeof(g_audio_last_asset)-1);
    g_audio_last_asset[sizeof(g_audio_last_asset)-1]='\0';
    if(audio_asset_is_bgm(name)) {
        // Only arm a transition when the requested BGM actually changes.
        if(strcmp(g_audio_last_bgm_asset,name)!=0) {
            strncpy(g_audio_last_bgm_asset,name,sizeof(g_audio_last_bgm_asset)-1);
            g_audio_last_bgm_asset[sizeof(g_audio_last_bgm_asset)-1]='\0';
            g_audio_bgm_seen_ms=real_monotonic_ms();
            g_audio_bgm_pending=true;
            g_audio_game_bgm_started=false;
        }
    } else if(audio_asset_is_sfx(name) && g_clet.started && g_audio_clet_started_ms) {
        const uint64_t now=real_monotonic_ms();
        // Ignore boot/preload traffic. Only recover effect requests once the
        // title has had time to enter normal interactive runtime.
        if(now-g_audio_clet_started_ms >= 4000) {
            strncpy(g_audio_last_sfx_asset,name,sizeof(g_audio_last_sfx_asset)-1);
            g_audio_last_sfx_asset[sizeof(g_audio_last_sfx_asset)-1]='\0';
            g_audio_sfx_seen_ms=now;
            g_audio_sfx_play_snapshot=g_audio_play_count;
            g_audio_sfx_pending=true;
        }
    }
}


static void audio_note_sound_request_internal(int id,int arg2,int flag) {
    if(id<0 || id>999) return;
    char path[32];
    snprintf(path,sizeof(path),"sound/%03d.mmf",id);
    static unsigned trace_budget=8;
    if(trace_budget) {
        printf("[SREQ] id=%03d a2=%d f=%d\n",id,arg2,flag);
        trace_budget--;
    }
    const uint64_t now=real_monotonic_ms();
    if(id>=100 && id<=199) {
        if(strcmp(g_audio_last_bgm_asset,path)!=0) {
            strncpy(g_audio_last_bgm_asset,path,sizeof(g_audio_last_bgm_asset)-1);
            g_audio_last_bgm_asset[sizeof(g_audio_last_bgm_asset)-1]='\0';
            g_audio_bgm_seen_ms=now;
            g_audio_bgm_pending=true;
            g_audio_game_bgm_started=false;
        }
        return;
    }
    if(id>=0 && id<100 && g_clet.started && g_audio_clet_started_ms) {
        // Exact runtime request from Zenonia's sound manager. This is stronger
        // than observing file opens because most effect MMFs are preloaded.
        if(now-g_audio_clet_started_ms >= 2500) {
            const bool same_pending=g_audio_sfx_pending && strcmp(g_audio_last_sfx_asset,path)==0;
            // Do not move the deadline forward every frame for the same effect.
            if(!same_pending) {
                strncpy(g_audio_last_sfx_asset,path,sizeof(g_audio_last_sfx_asset)-1);
                g_audio_last_sfx_asset[sizeof(g_audio_last_sfx_asset)-1]='\0';
                g_audio_sfx_seen_ms=now;
                g_audio_sfx_play_snapshot=g_audio_play_count;
                g_audio_sfx_pending=true;
            }
        }
    }
}

static void media_register_clip(DummyMediaClip *c) {
    if(!c) return;
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) if(!g_media_clips[i]) { g_media_clips[i]=c; return; }
}
static void media_unregister_clip(DummyMediaClip *c) {
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) if(g_media_clips[i]==c) { g_media_clips[i]=nullptr; return; }
}

static const char *runner_physical_name(uint32_t bit) {
    for(int i=0;i<UI_PHYSICAL_COUNT;i++) if(g_runner_physical_keys[i]==bit) return g_runner_physical_names[i];
    return "-";
}

static void runner_ui_log_push_line(const char *line) {
    if(!line || !*line) return;
    strncpy(g_ui_log_lines[g_ui_log_head],line,31);
    g_ui_log_lines[g_ui_log_head][31]='\0';
    g_ui_log_head=(g_ui_log_head+1)%UI_LOG_CAP;
    if(g_ui_log_count<UI_LOG_CAP) g_ui_log_count++;
    if(g_log_visible && g_ui_tab==UI_TAB_LOG) g_ui_dirty=true;
}

static void runner_ui_draw(void);

static void runner_ui_log_push_text(const char *text) {
    if(!text) return;
    char line[32]; int n=0;
    for(const char *s=text;;s++) {
        const char ch=*s;
        if(ch=='\n' || ch=='\0') {
            if(n>0) { line[n]='\0'; runner_ui_log_push_line(line); n=0; }
            if(ch=='\0') break;
        } else if(ch!='\r') {
            if(n<31) line[n++]=ch;
        }
    }
}

static int runner_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    buf[sizeof(buf)-1]='\0';
    // Runtime logs live in the in-RAM ring buffer. Do not write to the hidden
    // libnds console while gameplay is visible: the 16-bit SUB bitmap consumes
    // the full 128 KiB VRAM C address range and text writes would corrupt it.
    runner_ui_log_push_text(buf);
    if(g_log_visible) {
        // Runtime imports can log dozens of lines in one guest tick.  Redrawing
        // the SUB text console for every line produces visible black/white flashes
        // in melonDS.  Mark dirty here; wipi_poll() presents it at a bounded rate.
        g_ui_dirty=true;
    }
    return n;
}
#define printf runner_printf

static void runner_ui_defaults(void) {
    memset(g_ui_single_map,0,sizeof(g_ui_single_map));
    memset(g_ui_combo_map1,0,sizeof(g_ui_combo_map1));
    memset(g_ui_combo_map2,0,sizeof(g_ui_combo_map2));
    // Byte-for-byte logical target order and default assignments from the
    // original source, matching its README. Never bind bare L: L is a combo
    // modifier; the combo consumes A/B/X/Y to avoid duplicate events.
    g_ui_single_map[0]=KEY_A;       // EZ / FIRE / confirm (-5)
    g_ui_single_map[1]=KEY_B;       // CLR (-16)
    g_ui_single_map[7]=KEY_Y;       // *
    g_ui_single_map[8]=KEY_X;       // 0
    g_ui_single_map[9]=KEY_R;       // #
    g_ui_single_map[10]=KEY_START;  // Call (-10); quicksave is game-dependent
    const int targets[4]={2,3,5,6}; // 1,3,7,9
    const uint32_t keys[4]={KEY_A,KEY_B,KEY_X,KEY_Y};
    for(int i=0;i<4;i++) {
        g_ui_combo_map1[targets[i]]=KEY_L;
        g_ui_combo_map2[targets[i]]=keys[i];
    }
    g_bottom_half_visible=true;
    g_ui_prev_logical=0;
}

static uint32_t runner_first_physical(uint32_t mask) {
    for(int i=0;i<UI_PHYSICAL_COUNT;i++) if(mask&g_runner_physical_keys[i]) return g_runner_physical_keys[i];
    return 0;
}

static void runner_first_two_physical(uint32_t mask,uint32_t *a,uint32_t *b) {
    *a=0; *b=0;
    for(int i=0;i<UI_PHYSICAL_COUNT;i++) if(mask&g_runner_physical_keys[i]) {
        if(!*a) *a=g_runner_physical_keys[i];
        else if(!*b) { *b=g_runner_physical_keys[i]; return; }
    }
}

static void runner_format_mapping(char *out,size_t cap,int idx,bool combo) {
    if(!out||cap==0) return;
    if(combo) {
        if(g_ui_combo_map1[idx]&&g_ui_combo_map2[idx])
            snprintf(out,cap,"%s+%s",runner_physical_name(g_ui_combo_map1[idx]),runner_physical_name(g_ui_combo_map2[idx]));
        else snprintf(out,cap,"-");
    } else {
        if(g_ui_single_map[idx]) snprintf(out,cap,"%s",runner_physical_name(g_ui_single_map[idx]));
        else snprintf(out,cap,"-");
    }
    out[cap-1]='\0';
}

static void runner_ui_set_row(int row,const char *text) {
    char tmp[32];
    snprintf(tmp,sizeof(tmp),"%-31.31s",text?text:"");
    iprintf("\x1b[%d;0H%s",row,tmp);
}

static void runner_ui_draw(void) {
    if(!g_log_visible || !g_log_console || !g_ui_dirty) return;
    consoleSelect(g_log_console);
    // Every visible row is padded/overwritten below.  Do not clear the full
    // console first: the clear itself becomes a visible blank frame.
    char tabs[32];
    snprintf(tabs,sizeof(tabs),"%s %s %s [close]",
             g_ui_tab==UI_TAB_SINGLE?"[single]":" single ",
             g_ui_tab==UI_TAB_COMBO?"[combo]":" combo ",
             g_ui_tab==UI_TAB_LOG?"[log]":" log ");
    runner_ui_set_row(0,tabs);
    runner_ui_set_row(1,"D-pad logical keys stay fixed");

    if(g_ui_tab==UI_TAB_LOG) {
        ZenBootProfile::trace.active=false; // freeze when user first opens LOG
        char row[32];
        for(int i=0;i<13;++i) {
            ZenBootProfile::line(i,row,sizeof(row));runner_ui_set_row(2+i,row);
        }
        int start=g_ui_log_head-g_ui_log_count;if(start<0) start+=UI_LOG_CAP;
        for(int i=0;i<7;++i) {
            const int logical=g_ui_log_count-7+i;
            runner_ui_set_row(15+i,logical<0?"":g_ui_log_lines[(start+logical)%UI_LOG_CAP]);
        }
        runner_ui_set_row(22,"tap SINGLE/COMBO to remap");
        runner_ui_set_row(23,"keymap RAM-only; game save FAT");
    } else {
        runner_ui_set_row(1,g_ui_tab==UI_TAB_SINGLE ?
            (g_ui_capture_single>=0?"press one physical key":"tap a target row to remap") :
            (g_ui_capture_combo>=0?"hold two physical keys":"tap a target row to set combo"));
        for(int i=0;i<UI_TARGET_COUNT;i++) {
            char map[20],row[32];
            runner_format_mapping(map,sizeof(map),i,g_ui_tab==UI_TAB_COMBO);
            bool selected=(g_ui_tab==UI_TAB_SINGLE)?(i==g_ui_selected_single):(i==g_ui_selected_combo);
            snprintf(row,sizeof(row),"%c %-4s -> %-16s",selected?'>':' ',g_runner_targets[i].name,map);
            runner_ui_set_row(UI_TARGET_TOP/8+i*2,row);
            runner_ui_set_row(UI_TARGET_TOP/8+i*2+1,"");
        }
    }
    g_ui_dirty=false;
}

struct DummyMediaClip {
    uint32_t magic;
    uint32_t type;
    uint8_t *data;
    uint32_t data_len;
    uint32_t data_cap;
    int16_t *pcm;
    uint32_t pcm_samples;
    uint32_t sample_rate;
    int32_t volume;
    int32_t playing;
    int32_t sound_id;
    uint32_t callback;
    uint8_t decoded_kind; // 1=SMAF ATR/Yamaha ADPCM, 2=RIFF PCM, 3=SMAF MTR synth
    uint8_t repeat;
    uint8_t end_notified;
    uint8_t runner_owned;
    uint64_t end_due_ms;
    char source_name[20];
};

// v030: small persistent PCM cache for frequently repeated effect MMFs.
// MTR synthesis is CPU-heavy on ARM9; caching means an attack/menu sound is
// rendered once, then replayed directly without re-decoding on every request.
struct SfxCacheEntry {
    char name[20];
    int16_t *pcm;
    uint32_t samples;
    uint32_t rate;
    uint64_t last_play_ms;
    uint64_t busy_until_ms;
};
constexpr int SFX_CACHE_SLOTS=12;
static SfxCacheEntry g_sfx_cache[SFX_CACHE_SLOTS]{};
static uint32_t g_sfx_cache_hits=0;
static uint32_t g_sfx_cache_misses=0;

constexpr uint32_t MEDIA_MAGIC=0x4d444131u; // MDA1
constexpr uint32_t MEDIA_MAX_ENCODED = 2u*1024u*1024u;
constexpr uint32_t MEDIA_MAX_PCM_BYTES = 6u*1024u*1024u;
constexpr uint32_t MEDIA_SYNTH_RATE = 8000u;
constexpr uint32_t MEDIA_MAX_MIX_BYTES = 0u; // v031 mixes directly into PCM

struct TraceStats {
    uint32_t db_opens=0, db_reads=0, db_read_bytes=0, db_open_fail=0;
    uint32_t get_fb=0, init_ctx=0, set_ctx=0, put_pixel=0, draw_rect=0, fill_rect=0;
    uint32_t draw_image=0, draw_string=0, flush=0, repaint=0, create_image=0;
    uint32_t offscreen_create=0, offscreen_destroy=0, copy_fb=0, copy_area=0;
    uint32_t get_rgb=0, set_rgb=0, timer_set=0, timer_fire=0;
};
static TraceStats g_trace{};

struct DbSlot {
    char name[48];
    uint8_t *data;
    uint32_t len;
    uint32_t capacity;
    // LGT 0x190..0x1a0 is the WIPI filesystem API, not the database API.
    // A file handle has one shared position for both reads and writes.
    uint32_t cursor;
    int mode;
    bool used;
    bool packaged;
    bool persistent;
    bool dirty;
};
static DbSlot g_db[DB_SLOTS]{};

static bool db_is_persistent_name(const char *name) {
    return ZenSav::IsPersistentName(name);
}

static void storage_init_internal(void) {
    // Select the active persistence backend once: 128 KiB cart EEPROM for the
    // melonDS profile, or DLDI/FAT for the hardware profile.
    ZenSav::InitFatFallback();
    printf("[SAV] backend=%s; Ragnarok slots=3 + option + list\n",ZenSav::BackendName());
}

static int db_persistent_file_size(const char *name,uint32_t *out_len) {
    if(out_len) *out_len=0;
    if(!db_is_persistent_name(name)) return -12;
    uint32_t len=0;
    if(!ZenSav::Exists(name,&len)) return -12;
    if(out_len) *out_len=len;
    return 0;
}

static bool ragnarok_encoded_file_valid(const char *name,const uint8_t *data,uint32_t len) {
    if(!name || !data || len<4) return false;
    const char *base=name;
    for(const char *p=name; *p; ++p) if(*p=='/' || *p=='\\') base=p+1;
    const bool save=(strncmp(base,"save",4)==0 && base[4]>='0' && base[4]<='2' &&
                     (base[5]==0 || strcmp(base+5,".dat")==0 || strcmp(base+5,".sav")==0));
    const bool meta=!strcmp(base,"option") || !strcmp(base,"option.sav") || !strcmp(base,"list");
    if(!save && !meta) return false;
    const uint32_t encoded_len=(uint32_t)data[0] | ((uint32_t)data[2]<<8);
    if(encoded_len!=len) return false;
    uint8_t check=0;
    for(uint32_t i=4;i<len;i++) check^=(uint8_t)(data[i]^data[i%3u]^(uint8_t)i);
    return check==data[3];
}

static int db_load_persistent(DbSlot &d,const char *name) {
    if(!db_is_persistent_name(name)) return -12;
    uint8_t *mem=nullptr;
    uint32_t len=0;
    const int rc=ZenSav::Load(name,&mem,&len);
    if(rc!=0) return rc;
    free(d.data);
    d.data=mem;
    d.len=len;
    d.capacity=len;
    d.packaged=false;
    d.persistent=true;
    d.dirty=false;
    d.cursor=0;
    printf("[SAV] DB load %s (%lu bytes)\n",d.name,(unsigned long)len);
    if(!strncmp(d.name,"save",4) || !strcmp(d.name,"option") || !strcmp(d.name,"list"))
        printf("[SLOT] %s len=%lu guest-check=%s\n",d.name,(unsigned long)len,
               ragnarok_encoded_file_valid(d.name,d.data,d.len)?"OK":"BAD");
    return 0;
}

static int db_persist(DbSlot &d) {
    if(!d.persistent || !d.dirty) return 0;
    const int rc=ZenSav::Store(d.name,d.data,d.len);
    if(rc!=0) return rc;
    d.dirty=false;
    printf("[SAV] DB commit %s (%lu bytes) via %s\n",d.name,(unsigned long)d.len,ZenSav::BackendName());
    return 0;
}

static void storage_flush_internal(void) {
    for(int i=0;i<DB_SLOTS;i++) {
        if(g_db[i].used && g_db[i].persistent && g_db[i].dirty) db_persist(g_db[i]);
    }
    ZenSav::Flush();
}

// WIPI time is millisecond-resolution. v009 uses Calico's system tick counter:
// it is 64-bit, monotonic, and intended for timed events in libnds 2.x.
// This avoids dependence on the VBlank wait path that Zenonia 2 disrupts.
static uint64_t monotonic_ms(void) {
    if(!g_clock_started) return g_timer_clock_ms;
    const uint64_t ticks=tickGetCount()-g_tick_base;
    return (ticks*1000ULL*(uint64_t)GUEST_TIME_NUM) /
           ((uint64_t)TICK_FREQ*(uint64_t)GUEST_TIME_DEN);
}
static uint64_t real_monotonic_ms(void) {
    if(!g_clock_started) return g_timer_clock_ms;
    const uint64_t ticks=tickGetCount()-g_tick_base;
    return (ticks*1000ULL)/(uint64_t)TICK_FREQ;
}
static uint64_t now_ms(void) { return monotonic_ms(); }
static uint64_t timer_now_ms(void) { return monotonic_ms(); }

static bool valid_ptr(uint32_t p) {
    return p>=0x02000000u && p<0x04000000u;
}

static WipiFramebuffer *as_fb(uint32_t p) {
    if (!valid_ptr(p)) return nullptr;

    // LGT services expose a 16-byte framebuffer wrapper whose ptr_image field
    // points at the actual WIPICFramebuffer. Keep support for direct backing
    // descriptors too because this shim creates offscreen buffers internally.
    LgtFramebuffer *public_fb=(LgtFramebuffer*)(uintptr_t)p;
    WipiFramebuffer *fb=nullptr;
    if((public_fb->owned_image==0u || public_fb->owned_image==1u) && valid_ptr(public_fb->ptr_image)) {
        WipiFramebuffer *candidate=(WipiFramebuffer*)(uintptr_t)public_fb->ptr_image;
        if(candidate->buf && candidate->width && candidate->height && candidate->bpp) fb=candidate;
    }
    if(!fb) {
        fb=(WipiFramebuffer*)(uintptr_t)p;
        if (!fb->buf || fb->width==0 || fb->height==0 || fb->bpp==0) return nullptr;
    }

    // Redirect only screen storage in the scoped legacy minimap call.
    if(g_minimap_capture && fb->buf==(uint32_t)(uintptr_t)g_phone_fb){
        g_minimap_alias=*fb;g_minimap_alias.buf=(uint32_t)(uintptr_t)g_minimap_fb;
        return &g_minimap_alias;
    }
    return fb;
}

static uint16_t *fb_pixels16(WipiFramebuffer *fb) {
    if (!fb || fb->bpp!=16 || !valid_ptr(fb->buf)) return nullptr;
    return (uint16_t*)(uintptr_t)fb->buf;
}

static UnknownThunkEntry *find_unknown(uint32_t table, uint32_t index) {
    for (int i=0;i<g_thunk_count;i++) {
        if (g_unknown[i].table==table && g_unknown[i].index==index) return &g_unknown[i];
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// WIPI Input Method (IM) API, table 0x1FB indexes 0x12C..0x130.
// Zenonia 2 queries these during CB0 before it creates the first gameplay
// screen.  Returning the generic stub's zero for 0x12D makes the game
// dereference a NULL supported-mode table, so provide the standard WIPI IM
// surface even though the DSi port does not need a full Korean automata yet.
//
// Standard API order confirmed from WIPI C API examples:
//   0x12C MC_imGetSurpportModeCount(void)
//   0x12D MC_imGetSupportedModes(void)
//   0x12E MC_imSetCurrentMode(int mode)
//   0x12F MC_imGetCurrentMode(void)
//   0x130 MC_imHandleInput(char key,int type,char *buf1,int *size1,
//                          char *buf2,int *size2)
// ---------------------------------------------------------------------------
static char g_im_mode_en_l[] = "EN/L";
static char g_im_mode_en_s[] = "EN/S";
static char g_im_mode_n123[] = "N123";
static char g_im_mode_ko[]   = "KO";
static char *g_im_modes[] = {
    g_im_mode_en_l, g_im_mode_en_s, g_im_mode_n123, g_im_mode_ko
};
static int32_t g_im_current_mode=1; // EN/S is the least surprising default.
static uint32_t g_im_trace_budget=24;

extern "C" int32_t im_get_support_mode_count(void) {
    if(g_im_trace_budget) {
        printf("[IM] mode count=4\n");
        g_im_trace_budget--;
    }
    return 4;
}

extern "C" char **im_get_supported_modes(void) {
    if(g_im_trace_budget) {
        printf("[IM] modes=%p EN/L EN/S N123 KO\n",(void*)g_im_modes);
        g_im_trace_budget--;
    }
    return g_im_modes;
}

extern "C" int32_t im_set_current_mode(int32_t mode) {
    const int32_t old=g_im_current_mode;
    if(mode>=0 && mode<4) g_im_current_mode=mode;
    if(g_im_trace_budget) {
        printf("[IM] set mode %ld -> %ld\n",(long)old,(long)g_im_current_mode);
        g_im_trace_budget--;
    }
    return 0;
}

extern "C" int32_t im_get_current_mode(void) {
    if(g_im_trace_budget) {
        printf("[IM] get mode=%ld\n",(long)g_im_current_mode);
        g_im_trace_budget--;
    }
    return g_im_current_mode;
}

static inline bool im_writeable_ptr(const void *p) {
    const uintptr_t v=(uintptr_t)p;
    return v>=0x02000000u && v<0x04000000u;
}

extern "C" int32_t im_handle_input(int32_t key, int32_t type,
                                     char *buf1, int32_t *size1,
                                     char *buf2, int32_t *size2) {
    int32_t cap1=0, cap2=0;
    if(im_writeable_ptr(size1)) cap1=*size1;
    if(im_writeable_ptr(size2)) cap2=*size2;

    // WIPI expects size1/size2 to become the number of bytes actually emitted.
    // For ordinary gameplay keys the IME should not synthesize text, so start
    // with empty complete/incomplete buffers.  Printable keypad ASCII is passed
    // through as a minimal fallback for later name-entry screens.
    if(im_writeable_ptr(buf1) && cap1>0) buf1[0]='\0';
    if(im_writeable_ptr(buf2) && cap2>0) buf2[0]='\0';
    if(im_writeable_ptr(size1)) *size1=0;
    if(im_writeable_ptr(size2)) *size2=0;

    if(key>=0x20 && key<=0x7e && im_writeable_ptr(buf1) && cap1>0) {
        char ch=(char)key;
        if(g_im_current_mode==0 && ch>='a' && ch<='z') ch=(char)(ch-'a'+'A');
        if(g_im_current_mode==1 && ch>='A' && ch<='Z') ch=(char)(ch-'A'+'a');
        if(g_im_current_mode==2 && !(ch>='0' && ch<='9')) ch='\0';
        if(ch) {
            buf1[0]=ch;
            if(cap1>1) buf1[1]='\0';
            if(im_writeable_ptr(size1)) *size1=1;
        }
    }

    if(g_im_trace_budget) {
        printf("[IM] input k=%ld t=%ld out=%ld/%ld\n",
               (long)key,(long)type,
               (long)(im_writeable_ptr(size1)?*size1:0),
               (long)(im_writeable_ptr(size2)?*size2:0));
        g_im_trace_budget--;
    }
    return 0;
}

// Diagnostic wrapper for *resolved* imports.  Unknown imports already go
// through wipi_dynamic_stub(), but the failing Ragnarok timer currently enters
// a callback and never returns without touching an unknown import.  Route known
// imports through a tiny ARM interworking trampoline so we can print the last
// WIPI call made inside that callback without changing the target ABI.
extern "C" void wipi_trace_known_import(uint32_t key) {
    if(!g_trace_next_timer || g_known_trace_budget==0) return;
    const uint32_t table=key>>16, index=key&0xffffu;
    printf("[K] %03lx:%03lx\n",(unsigned long)table,(unsigned long)index);
    g_known_trace_budget--;
}

static KnownTraceThunkEntry *find_known_trace(uint32_t table,uint32_t index,void *target) {
    for(int i=0;i<g_known_trace_count;i++) {
        if(g_known_trace[i].table==table && g_known_trace[i].index==index && g_known_trace[i].target==target)
            return &g_known_trace[i];
    }
    return nullptr;
}

static void *make_known_trace_thunk(uint32_t table,uint32_t index,void *target) {
    if(!target) return nullptr;
    if(KnownTraceThunkEntry *e=find_known_trace(table,index,target)) return (void*)e->code;
    if(!g_known_trace_pool || g_known_trace_count>=KNOWN_TRACE_THUNK_SLOTS) return target;
    KnownTraceThunkEntry &e=g_known_trace[g_known_trace_count];
    e.table=table; e.index=index; e.target=target;
    // Ten ARM words per thunk.  Preserve r0-r4+lr (24 bytes keeps the stack
    // 8-byte aligned), call the logger, restore the original arguments, then
    // tail-branch to the real ARM/Thumb target.  The target therefore sees the
    // exact same register/stack ABI as before instrumentation.
    e.code=g_known_trace_pool + g_known_trace_count*10;
    uint32_t *c=e.code;
    c[0]=0xE92D401Fu; // stmdb sp!, {r0-r4,lr}
    c[1]=0xE59F0010u; // ldr r0,[pc,#16] -> key (word 7)
    c[2]=0xE59FC010u; // ldr ip,[pc,#16] -> logger (word 8)
    c[3]=0xE12FFF3Cu; // blx ip
    c[4]=0xE8BD401Fu; // ldmia sp!, {r0-r4,lr}
    c[5]=0xE59FC008u; // ldr ip,[pc,#8] -> target (word 9)
    c[6]=0xE12FFF1Cu; // bx ip
    c[7]=(table<<16)|(index&0xffffu);
    c[8]=(uint32_t)(uintptr_t)&wipi_trace_known_import;
    c[9]=(uint32_t)(uintptr_t)target;
    DC_FlushRange(c,10*sizeof(uint32_t));
    IC_InvalidateRange(c,10*sizeof(uint32_t));
    g_known_trace_count++;
    return (void*)c;
}

extern "C" uint32_t wipi_dynamic_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t key) {
    uint32_t table=key>>16, index=key&0xffffu;
    UnknownThunkEntry *e=find_unknown(table,index);
    const bool first=(!e || !e->called);
    const bool timer_trace=(g_trace_next_timer && g_unknown_trace_budget>0);
    if (first || timer_trace) {
        printf("[CALL?] %03lx:%03lx a=%08lx,%08lx,%08lx%s\n",
               (unsigned long)table,(unsigned long)index,
               (unsigned long)a0,(unsigned long)a1,(unsigned long)a2,
               timer_trace?" T":"");
        if(e) e->called=true;
        if(timer_trace) g_unknown_trace_budget--;
    }
    return 0;
}

// Make a tiny ARM interworking trampoline for every unresolved table:index.
// r0-r2 stay untouched; r3 becomes a packed import key, then BX reaches the
// common C handler. This fixes v002's misleading "last resolved import" log.
static void *make_unknown_thunk(uint32_t table, uint32_t index) {
    if (UnknownThunkEntry *e=find_unknown(table,index)) return (void*)e->code;
    if (!g_thunk_pool || g_thunk_count>=UNKNOWN_THUNK_SLOTS) return nullptr;
    UnknownThunkEntry &e=g_unknown[g_thunk_count];
    e.table=table; e.index=index; e.called=false;
    e.code=g_thunk_pool + g_thunk_count*5;
    uint32_t *c=e.code;
    c[0]=0xE59F3004u; // ldr r3,[pc,#4]  -> key at +12
    c[1]=0xE59FC004u; // ldr ip,[pc,#4]  -> handler at +16
    c[2]=0xE12FFF1Cu; // bx ip
    c[3]=(table<<16)|(index&0xffffu);
    c[4]=(uint32_t)(uintptr_t)&wipi_dynamic_stub;
    DC_FlushRange(c,5*sizeof(uint32_t));
    IC_InvalidateAll();
    g_thunk_count++;
    printf("[RES?] %03lx:%03lx\n",(unsigned long)table,(unsigned long)index);
    return (void*)c;
}

extern "C" uint32_t wipi_stub(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (g_fallback_stub_count++<8)
        printf("[STUB!] no thunk a=%08lx,%08lx\n",(unsigned long)a0,(unsigned long)a1);
    (void)a2;(void)a3;
    return 0;
}

extern "C" uint32_t clet_register(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a2;(void)a3;
    g_clet.register_arg0=a0; g_clet.register_arg1=a1; g_clet.registered=true;
    const uint32_t *p=reinterpret_cast<const uint32_t *>(a0);
    if (p && valid_ptr(a0)) {
        for (int i=0;i<6;i++) g_clet.callbacks[i]=p[i];
    }
    printf("[CLET] register a0=%08lx a1=%08lx\n", (unsigned long)a0,(unsigned long)a1);
    for (int i=0;i<6;i++) printf(" %d:%08lx%s",i,(unsigned long)g_clet.callbacks[i],(i&1)?"\n":"");
    return 0;
}

// LGT WIPIC 0x68 is still named Unk13 by current WIE and intentionally
// returns success. Keep it explicit so it cannot be confused with another import.
extern "C" int knl_unk68(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    static bool once=false;
    if(!once) { printf("[WIPI] 1fb:068 no-op\n"); once=true; }
    (void)a0;(void)a1;(void)a2;(void)a3;
    return 0;
}

// This title also resolves module 0x1f8:0x16 during bootstrap. It is separate
// from WIPIC table 0x1fb and is currently a safe success-only vendor hook.
extern "C" int vendor_1f8_016(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    static bool once=false;
    if(!once) { printf("[WIPI] 1f8:016 no-op\n"); once=true; }
    (void)a0;(void)a1;(void)a2;(void)a3;
    return 0;
}

extern "C" void *knl_alloc(uint32_t size) {
    void *p=size ? malloc(size) : nullptr;
    if(g_mem_trace_budget) { printf("[MEM] alloc %lu -> %p\n",(unsigned long)size,p); g_mem_trace_budget--; }
    return p;
}
extern "C" void *knl_calloc(uint32_t size) {
    void *p=size ? calloc(1,size) : nullptr;
    if(g_mem_trace_budget) { printf("[MEM] calloc %lu -> %p\n",(unsigned long)size,p); g_mem_trace_budget--; }
    return p;
}
extern "C" uint32_t knl_free(void *p) {
    uint32_t v=(uint32_t)(uintptr_t)p;
    if(g_mem_trace_budget) { printf("[MEM] free %p\n",p); g_mem_trace_budget--; }
    free(p); return v;
}
extern "C" int knl_total_mem(void) { return 12*1024*1024; }
extern "C" int knl_free_mem(void) { return 6*1024*1024; }
extern "C" uint64_t knl_current_time(void) { return now_ms(); }
extern "C" int knl_get_program_name(char *out, int cap) {
    const char *s="0002C004"; int n=(int)strlen(s)+1; if (!out || cap<n) return -18; memcpy(out,s,n); return 0;
}
extern "C" int knl_get_system_property(const char *id, char *out, uint32_t cap) {
    if (!id || !out || cap==0) return -9;
    const char *v=nullptr;
    if (!strcmp(id,"RSSILEVEL")) v="30";
    else if (!strcmp(id,"BATTERYLEVEL")) v="100";
    else if (!strcmp(id,"PHONEMODEL")) v="Nintendo DSi";
    else if (!strcmp(id,"VOLUMELEVEL")) v="100";
    else if (!strcmp(id,"MEDIADEVICES")) v="audio/x-smaf,audio/mmf,audio/wav";
    else if (!strcmp(id,"PHONENUMBER")) v="";
    else if (!strcmp(id,"MIN")) v="01000000000";
    else if (!strcmp(id,"ANNUN_CALL")||!strcmp(id,"ANNUN_SMS")||!strcmp(id,"ANNUN_SILENT")||!strcmp(id,"ANNUN_ALARM")||!strcmp(id,"ANNUN_SECURITY")||!strcmp(id,"CURRENTCH")||!strcmp(id,"AIRPLANE_MODE")||!strcmp(id,"ROAMING_AREA")||!strcmp(id,"DS_LOCK")) v="0";
    else return -9;
    size_t n=strlen(v)+1; if (n>cap) return -18; memcpy(out,v,n); return 0;
}
extern "C" int knl_set_system_property(const char *id, const char *value) {
    (void)id; (void)value;
    return 0;
}

struct BootCostScope {
    ZenBootProfile::Kind kind; uint32_t start; const char *name;
    BootCostScope(ZenBootProfile::Kind k,const char *n=nullptr):kind(k),
        start(ZenBootProfile::trace.active?zen_boot_clock_ms():0),name(n) {}
    ~BootCostScope() { ZenBootProfile::add(kind,start,name); }
};
extern "C" int knl_get_resource_id(const char *name, uint32_t *out_size) {
    if (out_size) *out_size=0;
    if (!name) return -12;
    BootCostScope profile(ZenBootProfile::Resource,name);
    audio_note_asset(name);
    uint32_t size=0;
    if(!embedded_asset_size(name,&size)) {
        static unsigned miss_log_count=0;
        const bool audio_miss = strstr(name,".mmf") || strstr(name,".MMF");
        if(audio_miss || miss_log_count<4) {
            printf("[RES!] miss %s\n",name);
            if(!audio_miss) miss_log_count++;
        }
        return -12;
    }
    if (out_size) *out_size=size;
    size_t n=strlen(name)+1; char *h=(char*)malloc(n); if (!h) return -3; memcpy(h,name,n);
    return (int)(uintptr_t)h;
}
extern "C" int knl_get_resource(int id, void *buf, uint32_t cap) {
    if (id<0 || !buf) return -9;
    const char *name=(const char*)(uintptr_t)(uint32_t)id;
    BootCostScope profile(ZenBootProfile::Resource,name);
    const int result=embedded_asset_read(name,buf,cap);
    if(result==-12) printf("[RES!] read miss\n");
    if(result==0) embedded_asset_release_cache();
    return result;
}
extern "C" int knl_printk(const char *fmt, uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if (!fmt) return 0;
    return printf(fmt,a0,a1,a2,a3);
}
extern "C" int knl_sprintk(char *dst,const char *fmt,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,uint32_t a4,uint32_t a5) {
    if(!dst||!fmt) return 0;
    return sprintf(dst,fmt,a0,a1,a2,a3,a4,a5);
}
extern "C" uint32_t knl_exit(int code) { printf("[WIPI] Exit(%d)\n",code); return 0; }


// LGT/WIPI 0x338: MC_uicGetTime(component, out[6]).  Ragnarok uses this
// directly when it builds the save-slot timestamp.  The six guest words are
// second, minute, hour, day, month-1, year-1900.  Leaving this import as the
// generic success stub leaves the caller's stack untouched, which is why the
// slot menu previously printed large integers instead of a date.
extern "C" void uic_get_time(uint32_t component, int32_t *out) {
    (void)component;
    if(!out) return;
    time_t now=time(nullptr);
    struct tm *t=localtime(&now);
    if(!t) {
        memset(out,0,6*sizeof(*out));
        return;
    }
    out[0]=t->tm_sec;
    out[1]=t->tm_min;
    out[2]=t->tm_hour;
    out[3]=t->tm_mday;
    out[4]=t->tm_mon;
    out[5]=t->tm_year;
}

extern "C" void knl_def_timer(void *timer, void (*callback)(void*,void*)) {
    if(g_timer_trace_budget) {
        printf("[TMR] def t=%p cb=%p\n",timer,(void*)callback);
        g_timer_trace_budget--;
    }
    // MCTimer is guest-owned opaque storage.  Do not write our callback into
    // it: v014/v015 could overwrite game state next to an embedded timer.
    for (int i=0;i<TIMER_SLOTS;i++) {
        if (g_timers[i].timer==timer) {
            g_timers[i].callback=callback;
            g_timers[i].param=nullptr;
            g_timers[i].next_fire_real_ms=0;
            g_timers[i].armed=false;
            return;
        }
    }
    for (int i=0;i<TIMER_SLOTS;i++) {
        if (g_timers[i].timer==nullptr) {
            g_timers[i].timer=timer;
            g_timers[i].callback=callback;
            g_timers[i].param=nullptr;
            g_timers[i].due_ms=0;
            g_timers[i].next_fire_real_ms=0;
            g_timers[i].armed=false;
            return;
        }
    }
    printf("[TMR!] no slot (%d)\n",TIMER_SLOTS);
}

// MC_knlSetTimer is the standard LGT/WIPI-C four-word ABI:
//   (timer, timeout_low, timeout_high, param)
// timeout is a 64-bit millisecond value.  The earlier Zenonia-specific shim
// incorrectly treated r2 as the callback parameter and r3 as a private owner
// pointer.  Ragnarok uses the standard ABI, so that loses the real callback
// parameter and can crash/return immediately on the first scheduled tick.
extern "C" void knl_set_timer(void *timer, uint32_t timeout_low,
                              uint32_t timeout_high, void *param) {
    const uint64_t timeout=((uint64_t)timeout_high<<32) | (uint64_t)timeout_low;
    const uint64_t now=timer_now_ms();
    const uint64_t due=(UINT64_MAX-now < timeout) ? UINT64_MAX : now+timeout;
    for (int i=0;i<TIMER_SLOTS;i++) if (g_timers[i].timer==timer) {
        g_timers[i].param=param;
        g_timers[i].due_ms=due;
        g_timers[i].armed=true;
        g_trace.timer_set++;
        if(g_timer_trace_budget) {
            printf("[TMR] set s=%d lo=%lu hi=%lu p=%p cb=%p now=%llu due=%llu\n",
                   i,(unsigned long)timeout_low,(unsigned long)timeout_high,param,
                   (void*)g_timers[i].callback,(unsigned long long)now,
                   (unsigned long long)due);
            g_timer_trace_budget--;
        }
        return;
    }
    // DefTimer must precede SetTimer.  Do not guess a callback from guest memory.
    printf("[TMR!] set unknown %p lo=%lu hi=%lu p=%p\n",timer,
           (unsigned long)timeout_low,(unsigned long)timeout_high,param);
}
extern "C" void knl_unset_timer(void *timer) {
    for (int i=0;i<TIMER_SLOTS;i++) if (g_timers[i].timer==timer) g_timers[i].armed=false;
}

// LGT framebuffer accessors receive the public framebuffer handle. The old
// Zenonia shim ignored that argument and always returned the screen surface;
// Ragnarok uses these accessors for image/offscreen surfaces as well.
extern "C" void *fb_ptr(uint32_t fbp) {
    WipiFramebuffer *fb=as_fb(fbp);
    return fb && valid_ptr(fb->buf) ? (void*)(uintptr_t)fb->buf : nullptr;
}
extern "C" int fb_w(uint32_t fbp) {
    WipiFramebuffer *fb=as_fb(fbp); return fb ? (int)fb->width : 0;
}
extern "C" int fb_h(uint32_t fbp) {
    WipiFramebuffer *fb=as_fb(fbp); return fb ? (int)fb->height : 0;
}
extern "C" int fb_bpl(uint32_t fbp) {
    if(!valid_ptr(fbp)) return 0;
    LgtFramebuffer *pub=(LgtFramebuffer*)(uintptr_t)fbp;
    // LGT returns zero BPL for owned offscreen/image surfaces.
    if((pub->owned_image==0u || pub->owned_image==1u) && valid_ptr(pub->ptr_image) && pub->owned_image!=0u) return 0;
    WipiFramebuffer *fb=as_fb(fbp); return fb ? (int)fb->bpl : 0;
}
extern "C" int fb_bpp(uint32_t) { return 16; }

extern "C" uint32_t gfx_get_screen_fb(int kind) {
    g_trace.get_fb++;
    if(g_gfx_trace_budget) { printf("[GFX] screen fb kind=%d -> %p\n",kind,(void*)&g_screen_wrapper); g_gfx_trace_budget--; }
    return kind==0 ? (uint32_t)(uintptr_t)&g_screen_wrapper : 0;
}

static RvImageAllocation *rv_image(uint32_t imgp) {
    if(!valid_ptr(imgp)) return nullptr;
    RvImageAllocation *im=(RvImageAllocation*)(uintptr_t)imgp;
    if(im->magic!=RV_IMAGE_MAGIC || !valid_ptr(im->image.ptr_image)) return nullptr;
    return im;
}

extern "C" uint32_t gfx_get_image_framebuffer(uint32_t imgp) {
    RvImageAllocation *im=rv_image(imgp);
    return im ? im->image.ptr_framebuffer : 0;
}

extern "C" int gfx_get_image_property(uint32_t imgp,int property) {
    RvImageAllocation *im=rv_image(imgp);
    if(!im) return 0;
    if(property==4) return (int)im->backing.width;
    if(property==5) return (int)im->backing.height;
    return 0;
}

static RvFramebufferAllocation *rv_framebuffer(uint32_t fbp) {
    if(!valid_ptr(fbp)) return nullptr;
    RvFramebufferAllocation *fb=(RvFramebufferAllocation*)(uintptr_t)fbp;
    if(fb->magic!=RV_FB_MAGIC || fb->framebuffer.ptr_graphics!=(uint32_t)(uintptr_t)&fb->view ||
       fb->framebuffer.ptr_image!=(uint32_t)(uintptr_t)&fb->backing) return nullptr;
    return fb;
}

extern "C" uint32_t gfx_create_offscreen_framebuffer(int width,int height) {
    if(width<=0 || height<=0 || width>1024 || height>1024) return 0;
    const size_t bytes=(size_t)width*(size_t)height*2u;
    const size_t alloc_bytes=sizeof(RvFramebufferAllocation)+bytes;
    RvFramebufferAllocation *fb=(RvFramebufferAllocation*)memalign(32,(alloc_bytes+31u)&~31u);
    if(!fb) return 0;
    memset(fb,0,sizeof(*fb));
    uint8_t *pixels=(uint8_t*)(fb+1);
    memset(pixels,0,bytes);
    fb->backing.width=(uint32_t)width; fb->backing.height=(uint32_t)height;
    fb->backing.bpl=(uint32_t)width*2u; fb->backing.bpp=16; fb->backing.buf=(uint32_t)(uintptr_t)pixels;
    fb->view.ptr_backing=(uint32_t)(uintptr_t)&fb->backing; fb->view.x=0; fb->view.y=0;
    fb->view.width=(uint32_t)width; fb->view.height=(uint32_t)height;
    fb->framebuffer.owned_image=1;
    fb->framebuffer.ptr_graphics=(uint32_t)(uintptr_t)&fb->view;
    fb->framebuffer.ptr_image=(uint32_t)(uintptr_t)&fb->backing;
    fb->framebuffer.screen_kind=0; fb->magic=RV_FB_MAGIC;
    g_trace.offscreen_create++;
    if(g_gfx_trace_budget) { printf("[GFX] offscreen + %dx%d -> %p\n",width,height,(void*)&fb->framebuffer); g_gfx_trace_budget--; }
    return (uint32_t)(uintptr_t)&fb->framebuffer;
}

extern "C" void gfx_destroy_offscreen_framebuffer(uint32_t fbp) {
    if(!valid_ptr(fbp) || fbp==(uint32_t)(uintptr_t)&g_screen_wrapper) return;
    if(RvFramebufferAllocation *fb=rv_framebuffer(fbp)) {
        fb->magic=0; g_trace.offscreen_destroy++;
        if(g_gfx_trace_budget) { printf("[GFX] offscreen - %08lx\n",(unsigned long)fbp); g_gfx_trace_budget--; }
        free(fb);
        return;
    }
    // Image framebuffers are owned by DestroyImage; never free their interior
    // wrapper through the offscreen API.
    RvImageAllocation *im=(RvImageAllocation*)((uint8_t*)(uintptr_t)fbp - offsetof(RvImageAllocation,framebuffer));
    if(valid_ptr((uint32_t)(uintptr_t)im) && im->magic==RV_IMAGE_MAGIC &&
       (uint32_t)(uintptr_t)&im->framebuffer==fbp) return;
}

extern "C" void gfx_init_context(WipiGraphicsContext *ctx) {
    g_trace.init_ctx++; if(!ctx) return;
    memset(ctx,0,sizeof(*ctx));
    ctx->clip_x1=0; ctx->clip_y1=0; ctx->clip_x2=0x7fff; ctx->clip_y2=0x7fff;
    ctx->foreground=0; ctx->background=0x00ffffffu; ctx->alpha=255; ctx->pixel_param=255;
}

extern "C" int gfx_set_context(WipiGraphicsContext *ctx,uint32_t op,uint32_t pv) {
    g_trace.set_ctx++; if(!ctx) return -9;
    switch(op) {
        case 0:
            if(valid_ptr(pv)) {
                const int32_t *v=(const int32_t*)(uintptr_t)pv;
                ctx->clip_x1=v[0]; ctx->clip_y1=v[1];
                ctx->clip_x2=v[0] + v[2] - 1; ctx->clip_y2=v[1] + v[3] - 1;
            }
            break;
        case 1: ctx->foreground=pv; break;
        case 2: ctx->background=pv; break;
        case 3: break;
        case 4: if(pv<=255) ctx->alpha=pv; break;
        case 5: ctx->pixel_op=pv; break;
        case 6: ctx->pixel_param=pv; break;
        case 7: ctx->font=pv; break;
        case 8: ctx->style=pv; break;
        case 9:
            if(pv==0) { ctx->pixel_op=0; ctx->xor_enabled=0; }
            else { ctx->alpha=0; ctx->pixel_op=1; ctx->xor_enabled=1; }
            break;
        case 10:
            if(valid_ptr(pv)) {
                const int32_t *v=(const int32_t*)(uintptr_t)pv;
                ctx->offset_x=v[0]; ctx->offset_y=v[1];
            }
            break;
        default: break;
    }
    return 0;
}

static inline bool gfx_clip_point(const WipiFramebuffer *fb,const WipiGraphicsContext *ctx,int x,int y) {
    if(!fb || !ctx || x<0 || y<0 || x>=(int)fb->width || y>=(int)fb->height) return false;
    if(x<ctx->clip_x1 || y<ctx->clip_y1 || x>ctx->clip_x2 || y>ctx->clip_y2) return false;
    return true;
}
static inline void gfx_store_pixel(WipiFramebuffer *fb,int x,int y,const WipiGraphicsContext *ctx,uint16_t color) {
    uint16_t *pix=fb_pixels16(fb); if(!pix || !gfx_clip_point(fb,ctx,x,y)) return;
    pix[y*(fb->bpl/2)+x]=color;
}

extern "C" int gfx_put_pixel(uint32_t fbp,int x,int y,const WipiGraphicsContext *ctx) {
    g_trace.put_pixel++; WipiFramebuffer *fb=as_fb(fbp); if(!fb||!ctx) return 0;
    x+=ctx->offset_x; y+=ctx->offset_y;
    gfx_store_pixel(fb,x,y,ctx,(uint16_t)ctx->foreground);
    return 0;
}

extern "C" int gfx_get_display_info(uint32_t reserved,WipiDisplayInfo *out) {
    (void)reserved; if(!out) return -9;
    out->bpp=16; out->depth=16; out->width=PHONE_W; out->height=PHONE_H; out->bpl=PHONE_W*2;
    out->color_type=1; out->red_mask=0xf800; out->green_mask=0x07e0; out->blue_mask=0x001f;
    return 1;
}
extern "C" int gfx_get_font(int face,int style,int size) { (void)face; (void)style; return size?size:12; }
extern "C" int gfx_get_font_height(int font) { return font>0 && font<64 ? font : 12; }
extern "C" int gfx_get_font_ascent(int font) { int h=gfx_get_font_height(font); return (h*3)/4; }
extern "C" int gfx_get_font_descent(int font) { int h=gfx_get_font_height(font); return h-gfx_get_font_ascent(font); }
extern "C" int gfx_get_string_width(int font,const uint8_t *str,int len) {
    if(!str) return 0; if(len<0) len=(int)strlen((const char*)str);
    int h=gfx_get_font_height(font); return len*((h+1)/2);
}
static bool gfx_flush_source_to_phone(uint32_t source) {
    if(!g_phone_fb || !valid_ptr(source)) return false;
    WipiFramebuffer *src=as_fb(source);
    uint16_t *sp=fb_pixels16(src);
    if(!src || !sp) return false;

    // LGT FlushLcd presents the framebuffer handle passed in r1.  Ragnarok
    // uses offscreen buffers for double buffering, so merely presenting the
    // screen wrapper leaves stale/alternating pixels on the DS LCD.
    int view_x=0, view_y=0;
    uint32_t view_w=src->width, view_h=src->height;
    LgtFramebuffer *pub=(LgtFramebuffer*)(uintptr_t)source;
    if((pub->owned_image==0u || pub->owned_image==1u) && valid_ptr(pub->ptr_graphics)) {
        const LgtGraphicsView *view=(const LgtGraphicsView*)(uintptr_t)pub->ptr_graphics;
        if(valid_ptr(view->ptr_backing) && view->width && view->height) {
            view_x=view->x; view_y=view->y; view_w=view->width; view_h=view->height;
        }
    }

    // Screen flushes already draw into g_phone_fb directly. Avoid copying a
    // framebuffer onto itself (and, critically, avoid clearing its source).
    if(sp==g_phone_fb && view_x==0 && view_y==0 &&
       view_w==PHONE_W && view_h==PHONE_H) return true;

    if(view_w>PHONE_W) view_w=PHONE_W;
    if(view_h>PHONE_H) view_h=PHONE_H;
    const int dst_x=(PHONE_W-(int)view_w)/2;
    const int dst_y=(PHONE_H-(int)view_h)/2;
    memset(g_phone_fb,0,PHONE_W*PHONE_H*2);
    const int src_stride=(int)(src->bpl/2u);
    for(uint32_t y=0;y<view_h;y++) {
        const int sy=view_y+(int)y;
        const int dy=dst_y+(int)y;
        if(sy<0 || sy>=(int)src->height || dy<0 || dy>=PHONE_H) continue;
        for(uint32_t x=0;x<view_w;x++) {
            const int sx=view_x+(int)x;
            const int dx=dst_x+(int)x;
            if(sx<0 || sx>=(int)src->width || dx<0 || dx>=PHONE_W) continue;
            g_phone_fb[dy*PHONE_W+dx]=sp[sy*src_stride+sx];
        }
    }
    return true;
}

extern "C" int gfx_flush(int kind, uint32_t source, int, int, uint32_t, uint32_t) {
    g_trace.flush++;
    if(kind!=0) return -9;
    // Match LGT/WIPI FlushLcd(kind, source, x, y, w, h): the region is ignored
    // by the reference handset implementation, but the source framebuffer is
    // semantically required.
    (void)gfx_flush_source_to_phone(source);
    g_present_pending=true;
    return 0;
}
extern "C" int gfx_repaint(int, int, int, int, int) {
    g_trace.repaint++; g_repaint_pending=true;
    if(g_trace_repaint_once) printf("[RPT] queued\n");
    return 0;
}
extern "C" uint16_t gfx_pixel_from_rgb(int r,int g,int b) {
    r&=255; g&=255; b&=255;
    return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
}
extern "C" int gfx_rgb_from_pixel(uint16_t p,int *r,int *g,int *b) {
    if(r) *r=((p>>11)&31)*255/31;
    if(g) *g=((p>>5)&63)*255/63;
    if(b) *b=(p&31)*255/31;
    return p;
}

extern "C" void gfx_fill_rect(uint32_t fbp,int x,int y,int w,int h,const WipiGraphicsContext *ctx) {
    g_trace.fill_rect++; WipiFramebuffer *fb=as_fb(fbp); uint16_t *pix=fb_pixels16(fb); if(!pix||!ctx||w<=0||h<=0) return;
    x+=ctx->offset_x; y+=ctx->offset_y;
    int l=x, t=y, r=x+w, b=y+h;
    if(l<0)l=0; if(t<0)t=0; if(r>(int)fb->width)r=(int)fb->width; if(b>(int)fb->height)b=(int)fb->height;
    if(l<ctx->clip_x1)l=ctx->clip_x1; if(t<ctx->clip_y1)t=ctx->clip_y1;
    if(r>ctx->clip_x2+1)r=ctx->clip_x2+1; if(b>ctx->clip_y2+1)b=ctx->clip_y2+1;
    uint16_t c=(uint16_t)ctx->foreground; int stride=(int)(fb->bpl/2);
    for(int yy=t;yy<b;yy++) for(int xx=l;xx<r;xx++) pix[yy*stride+xx]=c;
}
extern "C" void gfx_draw_rect(uint32_t fbp,int x,int y,int w,int h,const WipiGraphicsContext *ctx) {
    g_trace.draw_rect++; if(w<=0||h<=0||!ctx)return;
    WipiFramebuffer *fb=as_fb(fbp); if(!fb)return; x+=ctx->offset_x; y+=ctx->offset_y;
    const uint16_t c=(uint16_t)ctx->foreground;
    for(int xx=0;xx<w;xx++){ gfx_store_pixel(fb,x+xx,y,ctx,c); gfx_store_pixel(fb,x+xx,y+h-1,ctx,c); }
    for(int yy=0;yy<h;yy++){ gfx_store_pixel(fb,x,y+yy,ctx,c); gfx_store_pixel(fb,x+w-1,y+yy,ctx,c); }
}
extern "C" void gfx_draw_line(uint32_t fbp,int x0,int y0,int x1,int y1,const WipiGraphicsContext *ctx) {
    WipiFramebuffer *fb=as_fb(fbp); if(!fb||!ctx)return; x0+=ctx->offset_x; y0+=ctx->offset_y; x1+=ctx->offset_x; y1+=ctx->offset_y;
    int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy; uint16_t c=(uint16_t)ctx->foreground;
    for(;;){ gfx_store_pixel(fb,x0,y0,ctx,c); if(x0==x1&&y0==y1)break; int e2=err*2; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
}

static void gfx_blit_fb(WipiFramebuffer *dst,int dx,int dy,int w,int h,WipiFramebuffer *src,int sx,int sy,const WipiGraphicsContext *ctx) {
    uint16_t *dp=fb_pixels16(dst), *sp=fb_pixels16(src); if(!dp||!sp||!ctx||w<=0||h<=0)return;
    dx+=ctx->offset_x; dy+=ctx->offset_y;
    // Clip once per rectangle, not once per pixel. This is a raw opaque
    // framebuffer copy, matching the previous WIPI blit semantics.
    int left=0, top=0, right=w, bottom=h;
    if(left < -sx) left=-sx;
    if(left < -dx) left=-dx;
    if(left < ctx->clip_x1-dx) left=ctx->clip_x1-dx;
    if(top < -sy) top=-sy;
    if(top < -dy) top=-dy;
    if(top < ctx->clip_y1-dy) top=ctx->clip_y1-dy;
    if(right > (int)src->width-sx) right=(int)src->width-sx;
    if(right > (int)dst->width-dx) right=(int)dst->width-dx;
    if(right > ctx->clip_x2-dx+1) right=ctx->clip_x2-dx+1;
    if(bottom > (int)src->height-sy) bottom=(int)src->height-sy;
    if(bottom > (int)dst->height-dy) bottom=(int)dst->height-dy;
    if(bottom > ctx->clip_y2-dy+1) bottom=ctx->clip_y2-dy+1;
    if(left>=right || top>=bottom) return;
    const int ds=dst->bpl/2, ss=src->bpl/2;
    const size_t bytes=(size_t)(right-left)*2u;
    // Aliased descriptors also need overlap protection. Copy bottom-up when
    // moving down; memmove handles horizontal overlap without heap allocation.
    const bool alias=(dp==sp);
    if(alias && ds!=ss) {
        // Unusual aliased views with different strides: preserve all source rows.
        const int rows=bottom-top;
        uint16_t *snapshot=(uint16_t*)malloc(bytes*(size_t)rows);
        if(!snapshot) return;
        for(int y=0;y<rows;++y)
            memcpy((uint8_t*)snapshot+(size_t)y*bytes,sp+(sy+top+y)*ss+sx+left,bytes);
        for(int y=0;y<rows;++y)
            memcpy(dp+(dy+top+y)*ds+dx+left,(uint8_t*)snapshot+(size_t)y*bytes,bytes);
        free(snapshot);
        return;
    }
    const bool reverse=alias && dy>sy;
    for(int yy=reverse?bottom-1:top; reverse?yy>=top:yy<bottom; yy+=reverse?-1:1) {
        uint16_t *out=dp+(dy+yy)*ds+dx+left;
        const uint16_t *in=sp+(sy+yy)*ss+sx+left;
        if(alias) memmove(out,in,bytes);
        else memcpy(out,in,bytes);
    }
}

static inline uint16_t gfx_blend565(uint16_t dst,uint16_t src,uint32_t a) {
    if(a>=255u) return src;
    if(a==0u) return dst;
    const uint32_t sr=(src>>11)&31u, sg=(src>>5)&63u, sb=src&31u;
    const uint32_t dr=(dst>>11)&31u, dg=(dst>>5)&63u, db=dst&31u;
    const uint32_t ia=255u-a;
    const uint32_t r=(sr*a+dr*ia+127u)/255u;
    const uint32_t g=(sg*a+dg*ia+127u)/255u;
    const uint32_t b=(sb*a+db*ia+127u)/255u;
    return (uint16_t)((r<<11)|(g<<5)|b);
}

static void gfx_blit_image(WipiFramebuffer *dst,int dx,int dy,int w,int h,RvImageAllocation *im,int sx,int sy,const WipiGraphicsContext *ctx) {
    if(!dst||!im||!ctx||w<=0||h<=0) return;
    uint16_t *dp=fb_pixels16(dst), *sp=fb_pixels16(&im->backing);
    if(!dp||!sp) return;
    const uint8_t *alpha=(im->mask.bpp==8 && valid_ptr(im->mask.buf)) ? (const uint8_t*)(uintptr_t)im->mask.buf : nullptr;
    const uint32_t global_alpha=ctx->alpha<=255u?ctx->alpha:255u;
    if(!alpha && global_alpha>=255u) { gfx_blit_fb(dst,dx,dy,w,h,&im->backing,sx,sy,ctx); return; }
    dx+=ctx->offset_x; dy+=ctx->offset_y;
    const int ds=dst->bpl/2, ss=im->backing.bpl/2, as=(int)im->mask.bpl;
    for(int yy=0;yy<h;yy++) {
        const int srcy=sy+yy, dsty=dy+yy;
        if(srcy<0 || srcy>=(int)im->backing.height || dsty<0 || dsty>=(int)dst->height) continue;
        if(dsty<ctx->clip_y1 || dsty>ctx->clip_y2) continue;
        for(int xx=0;xx<w;xx++) {
            const int srcx=sx+xx, dstx=dx+xx;
            if(srcx<0 || srcx>=(int)im->backing.width || dstx<0 || dstx>=(int)dst->width) continue;
            if(dstx<ctx->clip_x1 || dstx>ctx->clip_x2) continue;
            uint32_t a=alpha ? alpha[srcy*as+srcx] : 255u;
            a=(a*global_alpha+127u)/255u;
            if(a) dp[dsty*ds+dstx]=gfx_blend565(dp[dsty*ds+dstx],sp[srcy*ss+srcx],a);
        }
    }
}

extern "C" void gfx_draw_image(uint32_t fbp,int dx,int dy,int w,int h,uint32_t imgp,int sx,int sy,const WipiGraphicsContext *ctx) {
    g_trace.draw_image++; WipiFramebuffer *dst=as_fb(fbp); RvImageAllocation *im=rv_image(imgp);
    if(!dst||!ctx||!im) return;
    gfx_blit_image(dst,dx,dy,w,h,im,sx,sy,ctx);
}
extern "C" void gfx_copy_frame_buffer(uint32_t dstp,int dx,int dy,int w,int h,uint32_t srcp,int sx,int sy,const WipiGraphicsContext *ctx) {
    g_trace.copy_fb++; WipiFramebuffer *dst=as_fb(dstp),*src=as_fb(srcp); if(!dst||!src||!ctx)return; gfx_blit_fb(dst,dx,dy,w,h,src,sx,sy,ctx);
}
extern "C" void gfx_copy_area(uint32_t dstp,int dx,int dy,int w,int h,int sx,int sy,const WipiGraphicsContext *ctx) {
    g_trace.copy_area++; WipiFramebuffer *dst=as_fb(dstp); if(!dst||!ctx)return; gfx_blit_fb(dst,dx,dy,w,h,dst,sx+ctx->offset_x,sy+ctx->offset_y,ctx);
}
extern "C" void gfx_draw_arc(uint32_t fbp,int x,int y,int w,int h,int start_angle,int end_angle,const WipiGraphicsContext *ctx) {
    // Conservative boot implementation: keep bounds/clipping correct without
    // pulling floating-point libm into the ARM9 binary. Replace with a sampled
    // integer ellipse only if Zenonia later needs visible circular widgets.
    (void)start_angle; (void)end_angle; gfx_draw_rect(fbp,x,y,w,h,ctx);
}
extern "C" void gfx_fill_arc(uint32_t fbp,int x,int y,int w,int h,int start_angle,int end_angle,const WipiGraphicsContext *ctx) {
    (void)start_angle; (void)end_angle; gfx_fill_rect(fbp,x,y,w,h,ctx);
}
extern "C" void gfx_draw_string(uint32_t,int,int,const uint8_t*,int,const WipiGraphicsContext*) {
    g_trace.draw_string++; // Ragnarok carries game font resources; handset system-font rasterization is deferred.
}
extern "C" void gfx_get_rgb_pixels(uint32_t srcp,int x,int y,int w,int h,uint32_t dstp,int dst_bpl) {
    g_trace.get_rgb++; WipiFramebuffer *src=as_fb(srcp); uint16_t *sp=fb_pixels16(src); if(!sp||!valid_ptr(dstp)||w<=0||h<=0)return;
    uint32_t *dst=(uint32_t*)(uintptr_t)dstp; int ss=src->bpl/2; int stride=dst_bpl>0?dst_bpl/4:w;
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){int px=x+xx,py=y+yy;if(px<0||py<0||px>=(int)src->width||py>=(int)src->height)continue;uint16_t p=sp[py*ss+px];uint32_t r=((p>>11)&31)*255/31,g=((p>>5)&63)*255/63,b=(p&31)*255/31;dst[yy*stride+xx]=0xff000000u|(r<<16)|(g<<8)|b;}
}
extern "C" void gfx_set_rgb_pixels(uint32_t dstp,int x,int y,int w,int h,uint32_t srcp,int src_bpl,const WipiGraphicsContext *ctx) {
    g_trace.set_rgb++; WipiFramebuffer *dst=as_fb(dstp); uint16_t *dp=fb_pixels16(dst); if(!dp||!valid_ptr(srcp)||!ctx||w<=0||h<=0)return;
    const uint32_t *src=(const uint32_t*)(uintptr_t)srcp; int ds=dst->bpl/2; int stride=src_bpl>0?src_bpl/4:w;
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){int ox=x+xx+ctx->offset_x,oy=y+yy+ctx->offset_y;if(!gfx_clip_point(dst,ctx,ox,oy))continue;uint32_t c=src[yy*stride+xx];dp[oy*ds+ox]=(uint16_t)((((c>>16)&255)>>3)<<11|((((c>>8)&255)>>2)<<5)|((c&255)>>3));}
}
struct RvPngParsed {
    uint32_t width=0, height=0;
    uint32_t idat_len=0;
    uint32_t crc32=0, fnv1a=0;
    uint8_t bit_depth=0, color_type=0, interlace=0;
    const uint8_t *plte=nullptr;
    uint32_t plte_len=0;
    const uint8_t *trns=nullptr;
    uint32_t trns_len=0;
};

static inline uint32_t rv_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}

static uint32_t rv_crc32_update(uint32_t crc,const uint8_t *p,uint32_t n) {
    while(n--) {
        crc ^= *p++;
        for(int i=0;i<8;i++) crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);
    }
    return crc;
}

static bool rv_png_parse(const uint8_t *src,uint32_t len,RvPngParsed *out) {
    static const uint8_t sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if(!src||!out||len<33u||memcmp(src,sig,8)!=0) return false;
    RvPngParsed r{};
    uint32_t pos=8, crc=0xffffffffu, fnv=2166136261u;
    bool have_ihdr=false, have_iend=false;
    while(pos+12u<=len) {
        const uint32_t n=rv_be32(src+pos);
        if(n>len-pos-12u) return false;
        const uint8_t *type=src+pos+4u;
        const uint8_t *data=src+pos+8u;
        if(!memcmp(type,"IHDR",4)) {
            if(n!=13u) return false;
            r.width=rv_be32(data); r.height=rv_be32(data+4);
            r.bit_depth=data[8]; r.color_type=data[9];
            if(data[10]!=0 || data[11]!=0) return false;
            r.interlace=data[12]; have_ihdr=true;
        } else if(!memcmp(type,"PLTE",4)) {
            r.plte=data; r.plte_len=n;
        } else if(!memcmp(type,"tRNS",4)) {
            r.trns=data; r.trns_len=n;
        } else if(!memcmp(type,"IDAT",4)) {
            if(r.idat_len>0xffffffffu-n) return false;
            r.idat_len+=n;
            crc=rv_crc32_update(crc,data,n);
            for(uint32_t i=0;i<n;i++) { fnv^=data[i]; fnv*=16777619u; }
        } else if(!memcmp(type,"IEND",4)) {
            have_iend=true; break;
        }
        pos += 12u+n;
    }
    if(!have_ihdr||!have_iend||!r.plte||r.plte_len<3u||r.color_type!=3u||
       r.width==0u||r.height==0u||r.width>1024u||r.height>1024u||
       (r.bit_depth!=1u&&r.bit_depth!=2u&&r.bit_depth!=4u&&r.bit_depth!=8u)||
       r.interlace>1u||r.idat_len==0u) return false;
    r.crc32=crc^0xffffffffu; r.fnv1a=fnv;
    *out=r; return true;
}

static const RvPngCacheEntry *rv_png_cache_find(const RvPngParsed &p) {
    const uint32_t pixels=p.width*p.height;
    for(uint32_t i=0;i<kRvPngCacheEntryCount;i++) {
        const RvPngCacheEntry &e=kRvPngCacheEntries[i];
        if(e.crc32==p.crc32 && e.fnv1a==p.fnv1a && e.idat_len==p.idat_len &&
           e.width==p.width && e.height==p.height && e.bit_depth==p.bit_depth &&
           e.interlace==p.interlace && e.pixels==pixels) return &e;
    }
    return nullptr;
}

static RvImageAllocation *rv_alloc_image_storage(uint32_t width,uint32_t height,bool alpha) {
    if(width==0||height==0||width>1024u||height>1024u) return nullptr;
    const size_t count=(size_t)width*(size_t)height;
    const size_t pixel_bytes=count*2u;
    const size_t alpha_bytes=alpha?count:0u;
    const size_t alloc_bytes=sizeof(RvImageAllocation)+pixel_bytes+alpha_bytes;
    RvImageAllocation *im=(RvImageAllocation*)memalign(32,(alloc_bytes+31u)&~31u);
    if(!im) return nullptr;
    memset(im,0,sizeof(*im));
    uint8_t *payload=(uint8_t*)(im+1);
    im->backing.width=width; im->backing.height=height; im->backing.bpl=width*2u; im->backing.bpp=16;
    im->backing.buf=(uint32_t)(uintptr_t)payload;
    if(alpha) {
        im->mask.width=width; im->mask.height=height; im->mask.bpl=width; im->mask.bpp=8;
        im->mask.buf=(uint32_t)(uintptr_t)(payload+pixel_bytes);
    }
    im->view.ptr_backing=(uint32_t)(uintptr_t)&im->backing; im->view.x=0; im->view.y=0;
    im->view.width=width; im->view.height=height;
    im->framebuffer.owned_image=1;
    im->framebuffer.ptr_graphics=(uint32_t)(uintptr_t)&im->view;
    im->framebuffer.ptr_image=(uint32_t)(uintptr_t)&im->backing;
    im->framebuffer.screen_kind=0;
    im->image.ptr_image=(uint32_t)(uintptr_t)&im->backing;
    im->image.ptr_framebuffer=(uint32_t)(uintptr_t)&im->framebuffer;
    im->magic=RV_IMAGE_MAGIC;
    return im;
}

static int rv_png_decode_cached(uint32_t *out_image,const uint8_t *src,uint32_t len) {
    RvPngParsed p{};
    if(!rv_png_parse(src,len,&p)) return -9;
    const RvPngCacheEntry *entry=rv_png_cache_find(p);
    if(!entry) {
        printf("[PNG!] cache miss %lux%lu crc=%08lx fnv=%08lx id=%lu\n",
               (unsigned long)p.width,(unsigned long)p.height,(unsigned long)p.crc32,
               (unsigned long)p.fnv1a,(unsigned long)p.idat_len);
        return -12;
    }
    bool has_alpha=false;
    for(uint32_t i=0;i<p.trns_len;i++) if(p.trns[i]!=255u) { has_alpha=true; break; }
    RvImageAllocation *im=rv_alloc_image_storage(p.width,p.height,has_alpha);
    if(!im) return -3;
    uint16_t *pixels=(uint16_t*)(uintptr_t)im->backing.buf;
    uint8_t *alpha=has_alpha?(uint8_t*)(uintptr_t)im->mask.buf:nullptr;
    FILE *f=fopen("nitro:/_port/rv_png_indices.bin","rb");
    if(!f || fseek(f,(long)entry->offset,SEEK_SET)!=0) {
        if(f) fclose(f); im->magic=0; free(im);
        printf("[PNG!] cache file/read open failed\n");
        return -12;
    }
    const uint32_t palette_count=p.plte_len/3u;
    uint8_t idxbuf[512];
    uint32_t done=0;
    while(done<entry->pixels) {
        const uint32_t chunk=(entry->pixels-done>sizeof(idxbuf))?(uint32_t)sizeof(idxbuf):(entry->pixels-done);
        if(fread(idxbuf,1,chunk,f)!=chunk) {
            fclose(f); im->magic=0; free(im);
            printf("[PNG!] cache short read at %lu/%lu\n",(unsigned long)done,(unsigned long)entry->pixels);
            return -12;
        }
        for(uint32_t j=0;j<chunk;j++) {
            const uint32_t k=done+j;
            const uint32_t ix=idxbuf[j];
            uint8_t r=0,g=0,b=0,a=255;
            if(ix<palette_count) {
                const uint8_t *rgb=p.plte+ix*3u;
                r=rgb[0]; g=rgb[1]; b=rgb[2];
            }
            if(p.trns && ix<p.trns_len) a=p.trns[ix];
            pixels[k]=(uint16_t)((((uint16_t)r>>3)<<11)|(((uint16_t)g>>2)<<5)|((uint16_t)b>>3));
            if(alpha) alpha[k]=a;
        }
        done+=chunk;
    }
    fclose(f);
    *out_image=(uint32_t)(uintptr_t)&im->image;
    if(g_trace.create_image<=12) printf("[PNG] create %lux%lu pal=%lu alpha=%lu -> %p\n",
        (unsigned long)p.width,(unsigned long)p.height,(unsigned long)palette_count,
        (unsigned long)(has_alpha?1:0),(void*)&im->image);
    return 1;
}

extern "C" int gfx_create_image(uint32_t *out_image,uint32_t data,uint32_t offset,uint32_t len) {
    g_trace.create_image++;
    if(out_image) *out_image=0;
    if(!out_image || !valid_ptr(data) || len<12u) return -9;
    const uint64_t begin64=(uint64_t)data+(uint64_t)offset;
    const uint64_t end64=begin64+(uint64_t)len-1u;
    if(begin64>0xffffffffu || end64>0xffffffffu || !valid_ptr((uint32_t)begin64) || !valid_ptr((uint32_t)end64)) return -9;
    const uint8_t *src=(const uint8_t*)(uintptr_t)(uint32_t)begin64;

    // Ragnarok parses and mutates embedded PNG palettes before CreateImage.
    // Keep those bytes pristine and use the offline IDAT index cache here.
    static const uint8_t png_sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if(len>=8u && memcmp(src,png_sig,8)==0) return rv_png_decode_cached(out_image,src,len);

    // Retain RVP1 as a compatibility fallback for experiments/tools, but the
    // shipped Ragnarok NitroFS no longer rewrites PNG resources to this format.
    if(src[0]!='R' || src[1]!='V' || src[2]!='P' || src[3]!='1') {
        if(g_trace.create_image<=12) {
            printf("[IMG!] unsupported len=%lu sig=%02x%02x%02x%02x\n",
                   (unsigned long)len,src[0],src[1],src[2],src[3]);
        }
        return -9;
    }
    const uint32_t width=(uint32_t)src[4] | ((uint32_t)src[5]<<8);
    const uint32_t height=(uint32_t)src[6] | ((uint32_t)src[7]<<8);
    const uint32_t flags=(uint32_t)src[8] | ((uint32_t)src[9]<<8) | ((uint32_t)src[10]<<16) | ((uint32_t)src[11]<<24);
    const size_t count=(size_t)width*(size_t)height;
    const size_t pixel_bytes=count*2u;
    const size_t alpha_bytes=(flags&1u)?count:0u;
    const size_t need=12u+pixel_bytes+alpha_bytes;
    if(width==0 || height==0 || width>1024u || height>1024u || need>(size_t)len) return -9;
    RvImageAllocation *im=rv_alloc_image_storage(width,height,alpha_bytes!=0);
    if(!im) return -3;
    uint8_t *payload=(uint8_t*)(uintptr_t)im->backing.buf;
    memcpy(payload,src+12,pixel_bytes+alpha_bytes);
    *out_image=(uint32_t)(uintptr_t)&im->image;
    if(g_trace.create_image<=8) printf("[RVP] create %lux%lu alpha=%lu -> %p\n",
        (unsigned long)width,(unsigned long)height,(unsigned long)(alpha_bytes?1:0),(void*)&im->image);
    return 1;
}

extern "C" uint32_t knl_unk6a(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if(g_gfx_trace_budget){ printf("[STUB] 1fb:06a %08lx %08lx %08lx %08lx\n",(unsigned long)a0,(unsigned long)a1,(unsigned long)a2,(unsigned long)a3); g_gfx_trace_budget--; }
    return 0;
}
extern "C" void gfx_destroy_image(uint32_t imgp) {
    RvImageAllocation *im=rv_image(imgp);
    if(!im) return;
    if(g_gfx_trace_budget) { printf("[IMG] destroy %08lx\n",(unsigned long)imgp); g_gfx_trace_budget--; }
    im->magic=0;
    free(im);
}
extern "C" uint32_t gfx_unk_ee(uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if(g_gfx_trace_budget){ printf("[STUB] 1fb:0ee %08lx %08lx %08lx %08lx\n",(unsigned long)a0,(unsigned long)a1,(unsigned long)a2,(unsigned long)a3); g_gfx_trace_budget--; }
    return 0;
}


static int db_load_packaged(DbSlot &d,const char *name) {
    EmbeddedAssetView a{}; if(!embedded_asset_find(name,&a)) return -12;
    uint8_t *mem=(uint8_t*)malloc(a.size?a.size:1);
    if(!mem) return -3;
    if(a.size) memcpy(mem,a.data,a.size);
    embedded_asset_release_cache();
    free(d.data); d.data=mem; d.len=a.size; d.capacity=a.size; d.packaged=true;
    return 0;
}

// ---------------------------------------------------------------------------
// LGT/WIPI filesystem API (service ids 0x190..0x1a0).
//
// Older Zenonia runner revisions mislabeled these calls as a database API.
// Ragnarok uses them as ordinary files, including save0/option.  Preserve the
// packaged-resource overlay, but match the LGT file ABI: one shared file
// position, seek may move past EOF, file attributes are {0,0,size}, and the
// method ids below retain their filesystem meanings.
// ---------------------------------------------------------------------------
constexpr int FS_OPEN_READ_ONLY      = 1;
constexpr int FS_OPEN_WRITE_ONLY     = 2;
constexpr int FS_OPEN_WRITE_TRUNCATE = 4;

static bool fs_can_read(const DbSlot &d) {
    return d.mode!=FS_OPEN_WRITE_ONLY && d.mode!=FS_OPEN_WRITE_TRUNCATE;
}
static bool fs_can_write(const DbSlot &d) {
    return d.mode!=FS_OPEN_READ_ONLY && !d.packaged;
}

extern "C" int db_open(const char *name,int mode,int type) {
    (void)type; g_trace.db_opens++; if(!name) return -9;
    audio_note_asset(name);

    // Match WIPI filesystem semantics: every MC_fsOpen creates a fresh handle.
    // In particular, slot-menu probes must reload save0/save1/save2 from EEPROM
    // rather than reusing stale in-RAM state from an earlier open/close cycle.

    for(int i=0;i<DB_SLOTS;i++) if(!g_db[i].used) {
        DbSlot &d=g_db[i]; memset(&d,0,sizeof(d)); d.used=true; d.mode=mode;
        strncpy(d.name,name,sizeof(d.name)-1);

        // Read-only/open-existing calls may resolve immutable packaged files.
        int load=(mode==FS_OPEN_WRITE_TRUNCATE)?-12:db_load_packaged(d,name);
        if(load==0) {
            d.mode=mode; d.cursor=0;
            printf("[FS] packaged %s len=%lu -> %d\n",d.name,(unsigned long)d.len,i+1);
            return i+1;
        }

        if(mode!=FS_OPEN_WRITE_TRUNCATE) {
            load=db_load_persistent(d,name);
            if(load==0) { d.mode=mode; d.cursor=0; return i+1; }
        }

        // WIPI mode 1 is open-existing. Missing file returns M_E_NOENT.
        if(mode==FS_OPEN_READ_ONLY) {
            memset(&d,0,sizeof(d));
            return -12;
        }

        // Other writable modes create an empty file if it does not exist.
        d.persistent=db_is_persistent_name(name);
        d.dirty=d.persistent;
        d.cursor=0;
        printf("[FS] %s %s mode=%d -> %d\n",d.persistent?"save":"ram",d.name,mode,i+1);
        return i+1;
    }
    g_trace.db_open_fail++;
    printf("[FS!] no free handle for %s (slots=%d)\n",name,DB_SLOTS);
    return -3;
}

extern "C" int db_read(int id,void *buf,uint32_t len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used||(!buf&&len)) return -2;
    g_trace.db_reads++;
    DbSlot &d=g_db[id-1];
    if(!fs_can_read(d)) return -24; // M_E_ACCESS
    if(len==0) return 0;
    const uint32_t before=d.cursor;
    if(d.cursor>=d.len) {
        if(g_db_trace_budget) { printf("[FSR] h=%d %s pos=%lu req=%lu -> EOF\n",id,d.name,(unsigned long)before,(unsigned long)len); g_db_trace_budget--; }
        return -23;
    }
    uint32_t n=d.len-d.cursor; if(n>len)n=len;
    if(n) memcpy(buf,d.data+d.cursor,n);
    d.cursor+=n; g_trace.db_read_bytes+=n;
    if(g_db_trace_budget) { printf("[FSR] h=%d %s %lu+%lu -> %lu\n",id,d.name,(unsigned long)before,(unsigned long)n,(unsigned long)d.cursor); g_db_trace_budget--; }
    return (int)n;
}

extern "C" int db_write(int id,const void *buf,uint32_t len) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used||(!buf&&len)) return -2;
    DbSlot &d=g_db[id-1];
    if(!fs_can_write(d)) return -24; // M_E_ACCESS
    if(len==0) return 0;
    if(d.cursor>0x7fffffffu-len) return -1;
    const uint32_t end=d.cursor+len;
    if(end>d.capacity) {
        uint32_t cap=d.capacity?d.capacity:64;
        while(cap<end) {
            if(cap>0x40000000u) { cap=end; break; }
            cap*=2;
        }
        uint8_t *p=(uint8_t*)realloc(d.data,cap); if(!p)return -3;
        if(cap>d.capacity) memset(p+d.capacity,0,cap-d.capacity);
        d.data=p; d.capacity=cap;
    }
    // Seeking beyond EOF creates a zero-filled hole, like WIE's filesystem.
    if(d.cursor>d.len) memset(d.data+d.len,0,d.cursor-d.len);
    memcpy(d.data+d.cursor,buf,len);
    d.cursor=end; if(end>d.len)d.len=end; d.packaged=false;
    if(d.persistent) d.dirty=true;
    return (int)len;
}

extern "C" int db_close(int id) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -2;
    DbSlot &d=g_db[id-1];
    int rc=0;
    if(d.persistent && d.dirty) {
        rc=db_persist(d);
        if(rc!=0) printf("[SAV!] close commit %s rc=%d\n",d.name,rc);
    }
    // A WIPI file descriptor is dead after close. Never leave a named cache
    // entry in g_db: a later open must observe the current persistent bytes.
    free(d.data);
    memset(&d,0,sizeof(d));
    return rc==0?0:rc;
}

extern "C" int db_seek(int id,int offset,int origin) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -2;
    DbSlot &d=g_db[id-1];
    int64_t base=(origin==0)?0:(origin==1?(int64_t)d.cursor:(origin==2?(int64_t)d.len:-1));
    if(base<0) return -1;
    const int64_t pos=base+(int64_t)offset;
    if(pos<0 || pos>0x7fffffffull) return -1;
    // Do NOT clamp at EOF. WIPI allows a later write to extend the file.
    d.cursor=(uint32_t)pos;
    if(g_db_trace_budget) { printf("[FSS] h=%d %s off=%d org=%d -> %ld\n",id,d.name,offset,origin,(long)pos); g_db_trace_budget--; }
    return (int)pos;
}

static int db_find_name(const char *name) {
    if(!name) return -1;
    for(int i=0;i<DB_SLOTS;i++) if(g_db[i].used && !strncmp(g_db[i].name,name,sizeof(g_db[i].name))) return i;
    return -1;
}

// MC_fsFileAttribute(name, out[3], access_mode): out = {flags0, flags1, size}.
extern "C" int fs_file_attribute(const char *name,uint32_t *out,int access_mode) {
    (void)access_mode;
    if(!name || !out) return -9;
    uint32_t len=0;
    const int idx=db_find_name(name);
    if(idx>=0) len=g_db[idx].len;
    else {
        EmbeddedAssetView a{};
        if(embedded_asset_find(name,&a)) len=a.size;
        else if(db_persistent_file_size(name,&len)!=0) return -12;
    }
    out[0]=0; out[1]=0; out[2]=len;
    return 0;
}

extern "C" int db_exists(const char *name,int access_mode) {
    (void)access_mode;
    if(!name) return -9;
    EmbeddedAssetView a{};
    bool exists=(db_find_name(name)>=0) || embedded_asset_find(name,&a);
    if(!exists) { uint32_t ignored=0; exists=(db_persistent_file_size(name,&ignored)==0); }
    return exists?0:-12;
}

extern "C" int db_delete(const char *name,int access_mode) {
    (void)access_mode;
    if(!name)return -9;
    int idx=db_find_name(name);
    if(idx>=0) { free(g_db[idx].data); memset(&g_db[idx],0,sizeof(g_db[idx])); }
    if(db_is_persistent_name(name)) {
        const int rc=ZenSav::Remove(name);
        printf("[SAV] delete %s rc=%d\n",name,rc);
        return (rc==0 || rc==-12)?0:rc;
    }
    // Packaged resources are read-only.
    EmbeddedAssetView a{}; if(embedded_asset_find(name,&a)) return -24;
    return idx>=0?0:-12;
}

extern "C" int fs_rename(const char *old_name,const char *new_name,int access_mode) {
    (void)access_mode;
    if(!old_name || !new_name) return -9;
    if(!db_is_persistent_name(old_name) || !db_is_persistent_name(new_name)) return -1;
    uint8_t *mem=nullptr; uint32_t len=0;
    const int rc=ZenSav::Load(old_name,&mem,&len); if(rc!=0) return rc;
    const int wr=ZenSav::Store(new_name,mem,len); free(mem); if(wr!=0) return wr;
    ZenSav::Remove(old_name);
    return 0;
}
extern "C" int fs_mkdir(const char*,int) { return -1; }
extern "C" int fs_rmdir(const char*,int) { return -1; }
extern "C" int fs_list(const char*,void*,int,int) { return -1; }
extern "C" int db_available_storage(void) { return 1024*1024; }
extern "C" int fs_tell(int id) {
    if(id<1||id>DB_SLOTS||!g_db[id-1].used) return -2;
    return (int)g_db[id-1].cursor;
}

// Standard WIPI database slot 12 (LGT method 0x19c): MC_dbListDataBase.
// Several WIPI titles use its no-argument return value as the number of
// bytes available in application database storage rather than as a literal
// database-list operation. Returning the generic stub value 0 makes Inotia 1
// immediately show its "not enough storage" dialog. Keep a deterministic
// 1 MiB quota for the RAM-backed diagnostic save layer.


extern "C" int std_atoi(const char *s) { return s?atoi(s):0; }
extern "C" char *std_strcpy(char *d,const char*s){return strcpy(d,s);}
extern "C" char *std_strncpy(char*d,const char*s,size_t n){return strncpy(d,s,n);}
extern "C" char *std_strcat(char*d,const char*s){return strcat(d,s);}
extern "C" int std_strcmp(const char*a,const char*b){return strcmp(a,b);}
extern "C" size_t std_strlen(const char*s){return strlen(s);}
extern "C" void *std_memcpy(void*d,const void*s,size_t n){return memcpy(d,s,n);}
extern "C" void *std_memset(void*d,int c,size_t n){return memset(d,c,n);}
// LGT guest libc uses a 32-bit time_t even when the host/newlib toolchain uses
// a different time_t width/layout.  Do not expose devkitARM's native time_t or
// struct tm directly to the guest: Ragnarok stores five tm fields in its slot
// metadata and later formats them as "%d/%d/%d %d:%d".  A host ABI mismatch
// turns those fields into large pointer/stack-looking integers.
struct GuestTm32 {
    uint32_t tm_sec;
    uint32_t tm_min;
    uint32_t tm_hour;
    uint32_t tm_mday;
    uint32_t tm_mon;
    uint32_t tm_year;
    uint32_t tm_wday;
    uint32_t tm_yday;
    uint32_t tm_isdst;
    uint32_t tm_gmtoff;
    uint32_t tm_zone;
};
static_assert(sizeof(GuestTm32)==0x2c,"LGT guest tm ABI must stay 44 bytes");
static GuestTm32 g_guest_tm32{};

extern "C" uint32_t std_time(uint32_t *out) {
    const time_t host_now=time(nullptr);
    const uint32_t epoch=(host_now<0)?0u:(uint32_t)(uint64_t)host_now;
    if(out) *out=epoch;
    return epoch;
}

extern "C" GuestTm32 *std_localtime(const uint32_t *guest_time) {
    if(!guest_time) return nullptr;
    // WIE's LGT reference runtime currently exposes localtime in KST (UTC+9).
    // Reproduce that ABI deterministically instead of depending on emulator/PC
    // timezone settings.
    time_t adjusted=(time_t)(uint64_t)(*guest_time);
    adjusted += (time_t)(9*60*60);
    struct tm *t=gmtime(&adjusted);
    if(!t) return nullptr;
    g_guest_tm32.tm_sec=(uint32_t)t->tm_sec;
    g_guest_tm32.tm_min=(uint32_t)t->tm_min;
    g_guest_tm32.tm_hour=(uint32_t)t->tm_hour;
    g_guest_tm32.tm_mday=(uint32_t)t->tm_mday;
    g_guest_tm32.tm_mon=(uint32_t)t->tm_mon;
    g_guest_tm32.tm_year=(uint32_t)t->tm_year;
    g_guest_tm32.tm_wday=(uint32_t)t->tm_wday;
    // WIE writes chrono ordinal here (1-based), not libc's 0-based tm_yday.
    g_guest_tm32.tm_yday=(uint32_t)t->tm_yday + 1u;
    g_guest_tm32.tm_isdst=0;
    g_guest_tm32.tm_gmtoff=9u*60u*60u;
    g_guest_tm32.tm_zone=0;
    return &g_guest_tm32;
}
extern "C" int std_sprintf(char *dst,const char *fmt,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,uint32_t a4,uint32_t a5) {
    if(!dst||!fmt) return 0;
    return sprintf(dst,fmt,a0,a1,a2,a3,a4,a5);
}
static uint32_t g_std_rand_state=1;
extern "C" int std_rand(void) {
    g_std_rand_state=g_std_rand_state*1103515245u+12345u;
    return (int)((g_std_rand_state>>16)&0x7fffu);
}
extern "C" void std_srand(uint32_t seed) { g_std_rand_state=seed; }
extern "C" char *std_strstr(const char *hay,const char *needle) {
    if(!hay||!needle) return nullptr;
    return (char*)strstr(hay,needle);
}
extern "C" int std_unk2(uint32_t) { return 0; }
extern "C" int std_unk4(uint32_t,uint32_t,uint32_t,uint32_t) { return 0; }
extern "C" uint32_t std_unk3(uint32_t fn) {
    if(!valid_ptr(fn & ~1u)) return 0;
    typedef uint32_t (*Fn)(void);
    return ((Fn)(uintptr_t)fn)();
}

static DummyMediaClip *media_clip(uint32_t p) {
    if(!valid_ptr(p)) return nullptr;
    DummyMediaClip *c=(DummyMediaClip*)(uintptr_t)p;
    return c->magic==MEDIA_MAGIC?c:nullptr;
}

static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1]<<8));
}
static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline int16_t clip_s16(int v) {
    if(v>32767) return 32767;
    if(v<-32768) return -32768;
    return (int16_t)v;
}

struct YamahaState { int predictor; int step; };
static int16_t yamaha_expand_nibble(YamahaState &s, uint8_t nibble) {
    static const int16_t indexscale[16]={
        230,230,230,230,307,409,512,614,
        230,230,230,230,307,409,512,614
    };
    static const int8_t difflookup[16]={
        1,3,5,7,9,11,13,15,-1,-3,-5,-7,-9,-11,-13,-15
    };
    if(!s.step) { s.predictor=0; s.step=127; }
    s.predictor += (s.step * difflookup[nibble & 15]) / 8;
    if(s.predictor>32767) s.predictor=32767;
    if(s.predictor<-32768) s.predictor=-32768;
    s.step=(s.step * indexscale[nibble & 15]) >> 8;
    if(s.step<127) s.step=127;
    if(s.step>24576) s.step=24576;
    return (int16_t)s.predictor;
}

static const uint32_t kMidiPhase11025[128]={
    3185015u,3374406u,3575058u,3787642u,4012867u,4251485u,4504291u,4772130u,
    5055896u,5356535u,5675051u,6012507u,6370030u,6748811u,7150117u,7575285u,
    8025735u,8502970u,9008582u,9544261u,10111792u,10713070u,11350103u,12025015u,
    12740059u,13497623u,14300233u,15150569u,16051469u,17005939u,18017165u,19088521u,
    20223584u,21426141u,22700205u,24050030u,25480119u,26995246u,28600467u,30301139u,
    32102938u,34011878u,36034330u,38177043u,40447168u,42852281u,45400411u,48100060u,
    50960238u,53990491u,57200933u,60602278u,64205876u,68023757u,72068660u,76354085u,
    80894335u,85704563u,90800821u,96200119u,101920476u,107980983u,114401866u,121204555u,
    128411753u,136047513u,144137319u,152708170u,161788671u,171409126u,181601643u,192400238u,
    203840952u,215961966u,228803732u,242409110u,256823506u,272095026u,288274639u,305416341u,
    323577341u,342818251u,363203285u,384800477u,407681904u,431923931u,457607465u,484818220u,
    513647012u,544190053u,576549277u,610832681u,647154683u,685636503u,726406571u,769600953u,
    815363807u,863847862u,915214929u,969636441u,1027294024u,1088380105u,1153098554u,1221665363u,
    1294309365u,1371273005u,1452813141u,1539201906u,1630727614u,1727695724u,1830429858u,1939272882u,
    2054588048u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,
    2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u,2147483647u
};

static void media_release_pcm(DummyMediaClip *c) {
    if(!c) return;
    if(c->sound_id>=0) { soundKill(c->sound_id); c->sound_id=-1; }
    if(c->pcm) { free(c->pcm); c->pcm=nullptr; }
    c->pcm_samples=0;
    c->sample_rate=0;
    c->decoded_kind=0;
    c->playing=0;
}

static void media_stop_runner_duplicates(const char *source_name) {
    if(!source_name || !*source_name) return;
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) {
        DummyMediaClip *d=g_media_clips[i];
        if(!d || d->magic!=MEDIA_MAGIC || !d->runner_owned) continue;
        if(strcmp(d->source_name,source_name)!=0) continue;
        if(d==g_audio_fallback_bgm) g_audio_fallback_bgm=nullptr;
        media_release_pcm(d);
        g_media_clips[i]=nullptr;
        if(d->data) free(d->data);
        d->data=nullptr; d->magic=0; free(d);
    }
}

static void media_stop_previous_bgm(DummyMediaClip *next) {
    DummyMediaClip *old=g_audio_active_bgm;
    if(!old || old==next || old->magic!=MEDIA_MAGIC) return;
    if(old->sound_id>=0) soundKill(old->sound_id);
    old->sound_id=-1;
    old->playing=0;
    old->end_notified=1;
    // Do not send STOP here: this is an internal single-owner handoff and the
    // game may be in the middle of replacing the clip already.
    g_audio_active_bgm=nullptr;
}


static int smaf_timebase_ms(uint8_t code) {
    switch(code) {
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

static bool smaf_read_vlq(const uint8_t *b,uint32_t n,uint32_t &p,uint32_t &out) {
    out=0;
    for(int i=0;i<4;i++) {
        if(p>=n) return false;
        uint8_t c=b[p++];
        out=(out<<7)|(uint32_t)(c&0x7f);
        if(!(c&0x80)) return true;
    }
    return false;
}

static bool smaf_find_mtsq(const uint8_t *mtr,uint32_t mtr_size,
                           const uint8_t **out_seq,uint32_t *out_size,
                           uint8_t *out_tbd,uint8_t *out_tbg,
                           uint8_t out_chstat[16]) {
    if(!mtr || mtr_size<20 || mtr[0]!=0x02) return false; // Mobile Standard, no compression
    if(out_tbd) *out_tbd=mtr[2];
    if(out_tbg) *out_tbg=mtr[3];
    if(out_chstat) memcpy(out_chstat,mtr+4,16);
    uint32_t p=20;
    while(p+8<=mtr_size) {
        uint32_t sz=read_be32(mtr+p+4);
        uint32_t payload=p+8;
        if(payload>mtr_size || sz>mtr_size-payload) break;
        if(!memcmp(mtr+p,"Mtsq",4)) {
            if(out_seq) *out_seq=mtr+payload;
            if(out_size) *out_size=sz;
            return true;
        }
        p=payload+sz;
    }
    return false;
}

struct SmafSynthState {
    uint8_t program[16];
    uint8_t volume[16];
    uint8_t expression[16];
    uint8_t velocity[16];
};

static void smaf_synth_state_init(SmafSynthState &s) {
    memset(&s,0,sizeof(s));
    for(int i=0;i<16;i++) {
        s.volume[i]=127;
        s.expression[i]=127;
        s.velocity[i]=64;
    }
}

static inline int32_t synth_wave(uint32_t phase,uint8_t program) {
    const uint16_t u=(uint16_t)(phase>>16);
    const int32_t saw=(int16_t)u;
    const int32_t tri=(u<32768u) ? ((int32_t)u*2-32768) : (98303-(int32_t)u*2);
    const int32_t sq=(phase&0x80000000u)?-32767:32767;
    switch(program>>3) {
        case 0: return (tri*3+saw)/4;             // piano
        case 1: return tri;                       // chromatic
        case 2: return (sq*2+tri)/3;              // organ
        case 3: return (saw+tri)/2;               // guitar
        case 4: return (sq+tri*2)/3;              // bass
        case 5: return (saw*2+tri)/3;              // strings
        case 6: return (saw+tri*2)/3;              // ensemble
        case 7: return (sq+saw)/2;                 // brass
        case 8: return (sq+tri)/2;                 // reed
        case 9: return tri;                        // pipe
        case 10:return (sq+saw)/2;                 // synth lead
        case 11:return (tri*3+saw)/4;              // synth pad
        case 12:return saw;                        // synth fx
        case 13:return (sq+tri)/2;                 // ethnic
        case 14:return tri;                        // percussion-ish melodic
        default:return (tri+saw)/2;
    }
}

static void smaf_mix_note(int16_t *pcm,uint32_t total_samples,
                          uint64_t start_ms,uint64_t gate_ms,
                          uint8_t ch,uint8_t note,uint8_t vel,
                          uint8_t program,uint8_t volume,uint8_t expression,
                          uint32_t seed) {
    if(!pcm || note>127 || gate_ms==0) return;
    uint64_t s64=start_ms*(uint64_t)MEDIA_SYNTH_RATE/1000u;
    uint64_t n64=gate_ms*(uint64_t)MEDIA_SYNTH_RATE/1000u;
    if(n64<2) n64=2;
    if(s64>=total_samples) return;
    if(n64>total_samples-s64) n64=total_samples-s64;
    // Very long pad gates are expensive on ARM9 and sound fine with a gentle
    // two-second release in this lightweight synth. This bounds worst-case work.
    const uint32_t max_gate=MEDIA_SYNTH_RATE*2u;
    if(n64>max_gate) n64=max_gate;
    uint32_t start=(uint32_t)s64, count=(uint32_t)n64;
    int amp=2200;
    amp=amp*(int)(vel?vel:1)/127;
    amp=amp*(int)volume/127;
    amp=amp*(int)expression/127;
    if(amp<32) return;
    const uint32_t attack=(count<(MEDIA_SYNTH_RATE/160u))?count:(MEDIA_SYNTH_RATE/160u);
    const uint32_t release=(count<(MEDIA_SYNTH_RATE/32u))?count:(MEDIA_SYNTH_RATE/32u);
    uint32_t phase=seed*2654435761u;
    // kMidiPhase11025 is the phase increment for 11.025 kHz. Rescale it for
    // the 8 kHz v031 synth without any floating point at runtime.
    uint32_t step=(uint32_t)(((uint64_t)kMidiPhase11025[note]*11025u)/MEDIA_SYNTH_RATE);
    uint32_t noise=seed^((uint32_t)note<<16)^0x9e3779b9u;
    const bool drum=(ch==9);
    for(uint32_t i=0;i<count;i++) {
        int env=32767;
        if(attack && i<attack) env=(int)(i*32767u/attack);
        uint32_t left=count-i;
        if(release && left<release) {
            int r=(int)(left*32767u/release);
            if(r<env) env=r;
        }
        int32_t w;
        if(drum && note>=45) {
            noise^=noise<<13; noise^=noise>>17; noise^=noise<<5;
            w=(int16_t)(noise>>16);
            uint32_t decay_len=MEDIA_SYNTH_RATE/5u;
            uint32_t decay=(i<decay_len)?(decay_len-i):0;
            env=(int)((uint32_t)env*decay/decay_len);
        } else if(drum) {
            w=synth_wave(phase,32);
        } else {
            w=synth_wave(phase,program);
        }
        // Two staged Q15 multiplies keep all arithmetic 32-bit. The v030
        // version used 64-bit multiply/divide for every sample and could look
        // completely frozen for dense BGM tracks on the ARM9.
        int32_t v=(w*amp)>>15;
        v=(v*env)>>15;
        int32_t mixed=(int32_t)pcm[start+i]+v;
        pcm[start+i]=clip_s16(mixed);
        phase+=step;
    }
}

// Decode Inotia's Mobile Standard (FormatType=0x02) score tracks into a
// lightweight GM-like software synth. It is not a bit-exact Yamaha MA-3/MA-5
// FM emulation, but it restores melody, harmony, bass and percussion without
// needing a ROM or external soundfont.
static int media_decode_smaf_mtr(DummyMediaClip *c,const uint8_t *mtr,uint32_t mtr_size) {
    const uint8_t *seq=nullptr; uint32_t seq_size=0; uint8_t tbd=0,tbg=0,chstat[16]{};
    if(!smaf_find_mtsq(mtr,mtr_size,&seq,&seq_size,&tbd,&tbg,chstat)) return -2;
    const int dur_ms=smaf_timebase_ms(tbd), gate_ms=smaf_timebase_ms(tbg);
    if(!dur_ms || !gate_ms || !seq || !seq_size) return -2;

    // Pass 1: determine track length while validating the event stream.
    uint32_t p=0; uint64_t tick=0,max_end_ms=0; uint32_t note_count=0,event_count=0;
    while(p<seq_size) {
        uint32_t d=0; if(!smaf_read_vlq(seq,seq_size,p,d)) return -2;
        tick+=d; event_count++;
        if(p>=seq_size) break;
        uint8_t st=seq[p++], hi=st&0xf0;
        if(hi==0x80 || hi==0x90) {
            if(p>=seq_size) return -2;
            p++; // note number
            if(hi==0x90) { if(p>=seq_size) return -2; p++; }
            uint32_t gate=0; if(!smaf_read_vlq(seq,seq_size,p,gate)) return -2;
            uint64_t end=tick*(uint64_t)dur_ms + gate*(uint64_t)gate_ms;
            if(end>max_end_ms) max_end_ms=end;
            note_count++;
        } else if(hi==0xb0 || hi==0xe0) {
            if(p+2>seq_size) return -2; p+=2;
        } else if(hi==0xc0) {
            if(p>=seq_size) return -2; p++;
        } else if(st==0xf0) {
            uint32_t len=0; if(!smaf_read_vlq(seq,seq_size,p,len) || len>seq_size-p) return -2; p+=len;
        } else if(st==0xff) {
            if(p>=seq_size) return -2;
            uint8_t sub=seq[p++];
            if(sub==0x2f) { if(p<seq_size) p++; break; }
            if(sub!=0x00) return -2;
        } else return -2;
    }
    uint64_t timeline_ms=tick*(uint64_t)dur_ms;
    if(timeline_ms>max_end_ms) max_end_ms=timeline_ms;
    if(max_end_ms<100 || note_count==0) return -2;
    uint64_t total64=(max_end_ms*(uint64_t)MEDIA_SYNTH_RATE)/1000u + 2u;
    if(total64>0xffffffffu) return -3;
    uint32_t total=(uint32_t)total64;
    uint64_t pcm_bytes=(uint64_t)total*sizeof(int16_t);
    if(pcm_bytes>MEDIA_MAX_PCM_BYTES) {
        printf("[SND?] MTR synth too long %lu ms\n",(unsigned long)max_end_ms);
        return -3;
    }
    printf("[SND] MTR fast render start %lu ms notes=%lu\n",
           (unsigned long)max_end_ms,(unsigned long)note_count);
    // v031 mixes straight into a zeroed 16-bit buffer: less than half the RAM
    // of v030 and no large normalization pass after synthesis.
    int16_t *pcm=(int16_t*)memalign(32,(size_t)pcm_bytes);
    if(!pcm) return -3;
    memset(pcm,0,(size_t)pcm_bytes);

    // Pass 2: apply channel state and mix each gated note into the output.
    SmafSynthState state{}; smaf_synth_state_init(state);
    p=0; tick=0; uint32_t serial=1;
    while(p<seq_size) {
        uint32_t d=0; if(!smaf_read_vlq(seq,seq_size,p,d)) break;
        tick+=d;
        if(p>=seq_size) break;
        uint8_t st=seq[p++],hi=st&0xf0,ch=st&0x0f;
        if(hi==0x80 || hi==0x90) {
            if(p>=seq_size) break;
            uint8_t note=seq[p++];
            uint8_t vel=state.velocity[ch];
            if(hi==0x90) { if(p>=seq_size) break; vel=seq[p++]; state.velocity[ch]=vel; }
            uint32_t gate=0; if(!smaf_read_vlq(seq,seq_size,p,gate)) break;
            smaf_mix_note(pcm,total,tick*(uint64_t)dur_ms,gate*(uint64_t)gate_ms,
                          ch,note,vel,state.program[ch],state.volume[ch],state.expression[ch],serial++);
        } else if(hi==0xb0) {
            if(p+2>seq_size) break;
            uint8_t cc=seq[p++], val=seq[p++];
            if(cc==7) state.volume[ch]=val;
            else if(cc==11) state.expression[ch]=val;
        } else if(hi==0xc0) {
            if(p>=seq_size) break; state.program[ch]=seq[p++];
        } else if(hi==0xe0) {
            if(p+2>seq_size) break; p+=2; // pitch bend ignored in stage 2
        } else if(st==0xf0) {
            uint32_t len=0; if(!smaf_read_vlq(seq,seq_size,p,len) || len>seq_size-p) break; p+=len;
        } else if(st==0xff) {
            if(p>=seq_size) break; uint8_t sub=seq[p++];
            if(sub==0x2f) { if(p<seq_size) p++; break; }
            if(sub!=0x00) break;
        } else break;
    }

    media_release_pcm(c);
    c->pcm=pcm; c->pcm_samples=total; c->sample_rate=MEDIA_SYNTH_RATE; c->decoded_kind=3;
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    printf("[SND] SMAF MTR fast %lu ms notes=%lu events=%lu rate=%lu\n",
           (unsigned long)max_end_ms,(unsigned long)note_count,(unsigned long)event_count,
           (unsigned long)MEDIA_SYNTH_RATE);
    return 0;
}

// Decode Yamaha SMAF/MMF. Sampled ATRx + AwaX clips use Yamaha ADPCM;
// Mobile Standard uncompressed MTR score tracks are rendered by the lightweight
// v030 software synth above.
static int media_decode_smaf(DummyMediaClip *c) {
    if(!c || !c->data || c->data_len<16 || memcmp(c->data,"MMMD",4)!=0) return -1;
    const uint8_t *b=c->data;
    const uint32_t n=c->data_len;
    uint32_t p=8;
    const uint8_t *atr=nullptr, *awa=nullptr, *mtr=nullptr;
    uint32_t atr_size=0, awa_size=0, mtr_size=0;
    while(p+8<=n) {
        const uint8_t *tag=b+p;
        uint32_t size=read_be32(b+p+4);
        uint32_t payload=p+8;
        if(payload>n || size>n-payload) break;
        if(tag[0]=='M'&&tag[1]=='T'&&tag[2]=='R' && !mtr) {
            mtr=b+payload; mtr_size=size;
        }
        if(tag[0]=='A'&&tag[1]=='T'&&tag[2]=='R' && !atr) {
            atr=b+payload; atr_size=size;
        }
        p=payload+size;
    }
    // Audio-track SMAF remains the preferred path for sampled SFX.  Pure
    // score-track files (Inotia BGM 1..13, 37..39) fall through to the synth.
    if(!atr || atr_size<6) {
        if(mtr && mtr_size) return media_decode_smaf_mtr(c,mtr,mtr_size);
        return -1;
    }
    const uint8_t params=atr[2];
    const int format=(params>>4)&7;
    const int rate_code=params&15;
    const int channels=(params>>7)+1;
    static const int rates[5]={4000,8000,11025,22050,44100};
    if(format!=1 || rate_code<0 || rate_code>4 || channels<1 || channels>2) {
        printf("[SND?] SMAF ATR params fmt=%d rate=%d ch=%d\n",format,rate_code,channels);
        return -1;
    }
    uint32_t q=6;
    while(q+8<=atr_size) {
        const uint8_t *tag=atr+q;
        uint32_t size=read_be32(atr+q+4);
        uint32_t payload=q+8;
        if(payload>atr_size || size>atr_size-payload) break;
        if(tag[0]=='A'&&tag[1]=='w'&&tag[2]=='a') {
            awa=atr+payload; awa_size=size; break;
        }
        q=payload+size;
    }
    if(!awa || !awa_size) return -1;

    uint64_t out_samples = channels==1 ? (uint64_t)awa_size*2u : (uint64_t)awa_size;
    uint64_t out_bytes=out_samples*sizeof(int16_t);
    if(out_bytes>MEDIA_MAX_PCM_BYTES || out_samples==0) return -3;
    int16_t *pcm=(int16_t*)memalign(32,(size_t)out_bytes);
    if(!pcm) return -3;

    YamahaState st[2]={{0,0},{0,0}};
    uint32_t w=0;
    if(channels==1) {
        for(uint32_t i=0;i<awa_size;i++) {
            uint8_t v=awa[i];
            pcm[w++]=yamaha_expand_nibble(st[0],v&0x0f);
            pcm[w++]=yamaha_expand_nibble(st[0],v>>4);
        }
    } else {
        // Stereo SMAF stores one nibble per channel in each byte. Downmix to
        // mono because a WIPI clip maps most cleanly to one DS PCM channel.
        for(uint32_t i=0;i<awa_size;i++) {
            uint8_t v=awa[i];
            int l=yamaha_expand_nibble(st[0],v&0x0f);
            int r=yamaha_expand_nibble(st[1],v>>4);
            pcm[w++]=(int16_t)((l+r)/2);
        }
    }
    media_release_pcm(c);
    c->pcm=pcm;
    c->pcm_samples=w;
    c->sample_rate=(uint32_t)rates[rate_code];
    c->decoded_kind=1;
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    printf("[SND] SMAF ATR %lu Hz %s -> %lu samples\n",
           (unsigned long)c->sample_rate,channels==1?"mono":"stereo->mono",
           (unsigned long)c->pcm_samples);
    return 0;
}

static int media_decode_wav(DummyMediaClip *c) {
    if(!c || !c->data || c->data_len<44) return -1;
    const uint8_t *b=c->data; const uint32_t n=c->data_len;
    if(memcmp(b,"RIFF",4)!=0 || memcmp(b+8,"WAVE",4)!=0) return -1;
    const uint8_t *fmt=nullptr,*dat=nullptr; uint32_t fmt_sz=0,dat_sz=0;
    uint32_t p=12;
    while(p+8<=n) {
        uint32_t sz=read_le32(b+p+4), payload=p+8;
        if(payload>n || sz>n-payload) break;
        if(memcmp(b+p,"fmt ",4)==0) { fmt=b+payload; fmt_sz=sz; }
        else if(memcmp(b+p,"data",4)==0) { dat=b+payload; dat_sz=sz; }
        p=payload+sz+(sz&1u);
    }
    if(!fmt||fmt_sz<16||!dat||!dat_sz) return -1;
    const int codec=read_le16(fmt+0), channels=read_le16(fmt+2), bits=read_le16(fmt+14);
    const uint32_t rate=read_le32(fmt+4);
    if(codec!=1 || (channels!=1&&channels!=2) || (bits!=8&&bits!=16) || rate<1000 || rate>48000) return -1;
    uint32_t bytes_per_frame=(uint32_t)channels*(bits/8);
    uint32_t frames=dat_sz/bytes_per_frame;
    uint64_t out_bytes=(uint64_t)frames*2u;
    if(!frames || out_bytes>MEDIA_MAX_PCM_BYTES) return -3;
    int16_t *pcm=(int16_t*)memalign(32,(size_t)out_bytes); if(!pcm) return -3;
    for(uint32_t i=0;i<frames;i++) {
        int a=0,bv=0;
        if(bits==8) {
            a=((int)dat[i*bytes_per_frame]-128)<<8;
            if(channels==2) bv=((int)dat[i*bytes_per_frame+1]-128)<<8;
        } else {
            a=(int16_t)read_le16(dat+i*bytes_per_frame);
            if(channels==2) bv=(int16_t)read_le16(dat+i*bytes_per_frame+2);
        }
        pcm[i]=(int16_t)(channels==2 ? (a+bv)/2 : a);
    }
    media_release_pcm(c);
    c->pcm=pcm; c->pcm_samples=frames; c->sample_rate=rate; c->decoded_kind=2;
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    printf("[SND] RIFF PCM %lu Hz %d-bit %s -> %lu samples\n",
           (unsigned long)rate,bits,channels==1?"mono":"stereo->mono",(unsigned long)frames);
    return 0;
}

static int media_decode(DummyMediaClip *c) {
    BootCostScope profile(ZenBootProfile::Audio);
    if(!c || !c->data || !c->data_len) return -1;
    if(c->pcm && c->pcm_samples && c->sample_rate) return 0;
    // Ragnarok does not ship Zenonia 2's predecoded title108 sound cache.
    // Keep the generic WIPI SMAF (ATR/MTR) and RIFF/WAV decode paths below.
    if(c->data_len>=4 && memcmp(c->data,"MMMD",4)==0) return media_decode_smaf(c);
    if(c->data_len>=12 && memcmp(c->data,"RIFF",4)==0) return media_decode_wav(c);
    printf("[SND?] unsupported clip header %02x %02x %02x %02x len=%lu\n",
           c->data[0],c->data_len>1?c->data[1]:0,c->data_len>2?c->data[2]:0,c->data_len>3?c->data[3]:0,
           (unsigned long)c->data_len);
    return -2;
}

extern "C" uint32_t media_clip_create(uint32_t type,uint32_t buf_size,uint32_t callback) {
    DummyMediaClip *c=(DummyMediaClip*)calloc(1,sizeof(DummyMediaClip));
    if(!c) return 0;
    c->magic=MEDIA_MAGIC; c->type=type; c->volume=100; c->sound_id=-1; c->callback=callback;
    c->repeat=0; c->end_notified=0;
    if(g_audio_last_asset[0]) {
        strncpy(c->source_name,g_audio_last_asset,sizeof(c->source_name)-1);
        c->source_name[sizeof(c->source_name)-1]='\0';
    }
    media_register_clip(c);
    if(buf_size>0 && buf_size<=MEDIA_MAX_ENCODED) {
        c->data=(uint8_t*)malloc(buf_size);
        if(c->data) c->data_cap=buf_size;
    }
    g_audio_create_count++;
    if(g_audio_create_count<=8) {
        printf("[AUD] create #%lu %s %luB\n",(unsigned long)g_audio_create_count,
               c->source_name[0]?c->source_name:"(clip)",(unsigned long)buf_size);
    }
    return (uint32_t)(uintptr_t)c;
}
extern "C" int media_clip_free(uint32_t p) {
    if(!p) return 0;
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    g_audio_free_count++;
    if(c==g_audio_active_bgm) g_audio_active_bgm=nullptr;
    if(c==g_audio_fallback_bgm) g_audio_fallback_bgm=nullptr;
    media_unregister_clip(c);
    media_release_pcm(c);
    if(c->data) free(c->data);
    c->data=nullptr; c->magic=0; free(c); return 0;
}
extern "C" int media_clip_put_data(uint32_t p,uint32_t buf,uint32_t len) {
    DummyMediaClip *c=media_clip(p); if(!c || (len && !valid_ptr(buf))) return -1;
    if(!len) return 0;
    if(c->data_len > MEDIA_MAX_ENCODED-len) return -18;
    uint32_t need=c->data_len+len;
    if(need>c->data_cap) {
        uint32_t cap=c->data_cap?c->data_cap:1024;
        while(cap<need && cap<MEDIA_MAX_ENCODED) cap*=2;
        if(cap>MEDIA_MAX_ENCODED) cap=MEDIA_MAX_ENCODED;
        if(cap<need) return -18;
        uint8_t *q=(uint8_t*)realloc(c->data,cap); if(!q) return -3;
        c->data=q; c->data_cap=cap;
    }
    memcpy(c->data+c->data_len,(const void*)(uintptr_t)buf,len);
    c->data_len+=len;
    g_audio_bytes_count+=len;
    // Any new encoded bytes invalidate an older decoded copy.
    if(c->pcm) media_release_pcm(c);
    return (int)len;
}
extern "C" int media_clip_get_volume(uint32_t p) {
    DummyMediaClip *c=media_clip(p); return c?c->volume:0;
}
extern "C" int media_clip_set_volume(uint32_t p,int v) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    if(v<0) v=0;
    if(v>100) v=100;
    c->volume=v;
    g_audio_last_requested_volume=v;
    // v021 test path intentionally keeps DS output at full level. This lets us
    // distinguish a WIPI volume-semantics mismatch from a decoder/output fault.
    if(c->sound_id>=0) soundSetVolume(c->sound_id,127);
    return 0;
}
static uint32_t media_pcm_peak(const DummyMediaClip *c) {
    if(!c || !c->pcm || !c->pcm_samples) return 0;
    uint32_t peak=0;
    for(uint32_t i=0;i<c->pcm_samples;i++) {
        int32_t v=c->pcm[i]; if(v<0) v=-v;
        if((uint32_t)v>peak) peak=(uint32_t)v;
    }
    return peak;
}

static uint32_t media_normalize_for_ds(DummyMediaClip *c) {
    uint32_t peak=media_pcm_peak(c);
    if(!c || !c->pcm || !c->pcm_samples || peak==0) return peak;
    // The lightweight SMAF synth is deliberately conservative. On DS speakers
    // some tracks are effectively inaudible, so raise quiet PCM to a useful
    // peak while preserving already-loud sampled SFX.
    const uint32_t target=24000;
    if(peak<target) {
        uint32_t gain_q12=(target<<12)/peak;
        if(gain_q12>(8u<<12)) gain_q12=(8u<<12);
        for(uint32_t i=0;i<c->pcm_samples;i++) {
            int32_t v=(int32_t)(((int64_t)c->pcm[i]*gain_q12)>>12);
            c->pcm[i]=clip_s16(v);
        }
        DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
        peak=media_pcm_peak(c);
    }
    return peak;
}

static void media_notify(DummyMediaClip *c,int event,int parm=0) {
    if(!c || !c->callback) return;
    const uint32_t cb=c->callback;
    if(!valid_ptr(cb & ~1u)) return;
    typedef void (*MediaCb)(uint32_t,int,int);
    MediaCb f=(MediaCb)(uintptr_t)cb;
    // Guest callbacks execute on this same ARM9. Code writes are synchronized
    // at import/thunk/patch creation, not on every callback invocation.
#if ZEN_CONSERVATIVE_CALLBACK_CACHE
    DC_FlushAll(); IC_InvalidateAll();
#endif
    f((uint32_t)(uintptr_t)c,event,parm);
}

// Zenonia 2 native media ABI observations from binary.mod:
// 4C1 is called as (100,duration): WIPI Vibrator.on(level,duration), not clip prepare.
extern "C" int media_vibrator_on(int level,int duration) {
    (void)level; (void)duration;
    return 0;
}

// 4CE is called with the profile string "GENERAL" and its return value is
// immediately clamped/stored as the game's master media volume. Returning 0
// here (v023) muted the game at manager level before any SFX could play.
extern "C" int media_get_profile_volume(const char *profile) {
    (void)profile;
    return g_media_profile_volume;
}

// 4D1/4D2 are used as a set/get pair with device id 6. The returned byte is
// stored in Zenonia 2's audio-manager enable flag. Keep device 6 enabled.
extern "C" int media_set_device_enabled(uint32_t device,int enabled) {
    if(device==6) {
        g_media_device_enabled = enabled ? 1 : 0;
        if(g_media_device_enabled) {
            soundEnable();
            soundSetMixerVolume((g_media_profile_volume*127)/100);
        } else {
            soundSetMixerVolume(0);
        }
    }
    return 0;
}
extern "C" int media_get_device_enabled(uint32_t device) {
    if(device==6) return g_media_device_enabled;
    return 1;
}

extern "C" int media_clip_prepare(uint32_t p,uint32_t) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    g_audio_prepare_count++;
    const int rc=media_decode(c);
    if(rc==0 && c->pcm && c->pcm_samples) {
        g_audio_ok_count++;
        printf("[AUD] prepared kind=%u rate=%lu samples=%lu\n",
               (unsigned)c->decoded_kind,(unsigned long)c->sample_rate,(unsigned long)c->pcm_samples);
        return 0;
    }
    g_audio_fail_count++;
    printf("[AUD!] prepare rc=%d len=%lu\n",rc,(unsigned long)c->data_len);
    return rc?rc:-1;
}

static void media_stop_fallback_bgm(void) {
    DummyMediaClip *c=g_audio_fallback_bgm;
    if(!c) return;
    g_audio_fallback_bgm=nullptr;
    if(c->sound_id>=0) soundKill(c->sound_id);
    if(c==g_audio_active_bgm) g_audio_active_bgm=nullptr;
    c->sound_id=-1; c->playing=0; c->end_notified=1; c->end_due_ms=0;
    media_unregister_clip(c);
    media_release_pcm(c);
    if(c->data) free(c->data);
    c->data=nullptr; c->magic=0; free(c);
}

extern "C" int media_clip_play(uint32_t p,int repeat) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    const bool is_bgm=audio_asset_is_bgm(c->source_name);

    // Native Player.play always wins. Remove runner-owned copies of the exact
    // same MMF before starting this channel; this eliminates the delayed
    // fallback + native double-play that caused audible echo in v028.
    if(!c->runner_owned && c->source_name[0]) media_stop_runner_duplicates(c->source_name);
    if(!c->runner_owned && is_bgm) {
        g_audio_game_bgm_started=true;
        g_audio_bgm_pending=false;
        media_stop_previous_bgm(c);
        if(g_audio_fallback_bgm && g_audio_fallback_bgm!=c) media_stop_fallback_bgm();
    }

    g_audio_play_count++;
    if(media_decode(c)!=0 || !c->pcm || !c->pcm_samples) {
        g_audio_fail_count++;
        printf("[AUD!] decode fail len=%lu\n",(unsigned long)c->data_len);
        return -1;
    }
    if(c->sound_id>=0) soundKill(c->sound_id);

    g_audio_last_peak=media_normalize_for_ds(c);
    g_audio_last_rate=c->sample_rate;
    g_audio_last_samples=c->pcm_samples;
    g_audio_last_kind=c->decoded_kind;
    g_audio_last_requested_volume=c->volume;
    if(g_audio_last_peak==0) {
        g_audio_fail_count++;
        printf("[AUD!] decoded PCM is silent k=%u n=%lu\n",
               (unsigned)c->decoded_kind,(unsigned long)c->pcm_samples);
        return -4;
    }
    if(!g_media_device_enabled || g_media_profile_volume<=0) return -5;

    soundEnable();
    soundSetMixerVolume((g_media_profile_volume*127)/100);
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    const bool loop=(repeat!=0);
    c->repeat=loop?1:0;
    c->end_notified=0;
    c->end_due_ms=loop?0:(real_monotonic_ms()+
        ((uint64_t)c->pcm_samples*1000ULL)/(c->sample_rate?c->sample_rate:8000u)+24ULL);
    c->sound_id=soundPlaySample(c->pcm,SoundFormat_16Bit,c->pcm_samples*sizeof(int16_t),
                                (u16)c->sample_rate,127,64,loop,0);
    c->playing=(c->sound_id>=0)?(loop?2:1):0;
    g_audio_last_channel=c->sound_id;
    if(c->sound_id>=0) {
        soundSetVolume(c->sound_id,127);
        soundSetPan(c->sound_id,64);
        soundSetFreq(c->sound_id,(u16)c->sample_rate);
        if(is_bgm) g_audio_active_bgm=c;
        g_audio_ok_count++;
        g_audio_start_count++;
        if(g_audio_start_count<=12)
            printf("[AUD] PLAY %s ch=%d loop=%d\n",
                   c->source_name[0]?c->source_name:"(clip)",c->sound_id,loop?1:0);
        media_notify(c,2,0); // START
        return 0;
    }
    g_audio_fail_count++;
    return -3;
}
extern "C" int media_clip_stop(uint32_t p) {
    DummyMediaClip *c=media_clip(p); if(!c) return -9;
    g_audio_stop_count++;
    const bool was_playing=(c->playing!=0 || c->sound_id>=0);
    if(c->sound_id>=0) soundKill(c->sound_id);
    c->sound_id=-1; c->playing=0; c->end_notified=1;
    if(was_playing) media_notify(c,3,0); // WIPI PlayListener.STOP
    return 0;
}

// Zenonia 2's audio manager relies on the clip callback to clear its busy state.
// The callback wrapper in binary.mod consumes the event in r1; event 1 is the
// normal end-of-data path used by the game. Without this notification the first
// one-shot clip leaves the sound manager permanently occupied, blocking later
// SFX and BGM requests.
static void media_poll_end_events(void) {
    // v029: no ARM9<->ARM7 channel-state polling. soundGetActiveChannels() is
    // synchronous IPC and was causing a visible hitch every few dozen ms.
    // One-shots have deterministic PCM length, so notify END from an unscaled
    // real clock instead. Keep the PCM alive slightly longer than nominal.
    const uint64_t now=real_monotonic_ms();
    for(int i=0;i<MEDIA_CLIP_SLOTS;i++) {
        DummyMediaClip *c=g_media_clips[i];
        if(!c || c->magic!=MEDIA_MAGIC || c->playing!=1 || c->repeat || c->end_notified) continue;
        if(!c->end_due_ms || now<c->end_due_ms) continue;
        if(c->sound_id>=0) soundKill(c->sound_id);
        c->playing=0; c->sound_id=-1; c->end_due_ms=0; c->end_notified=1;
        g_audio_end_count++;
        if(c->callback) media_notify(c,1,0); // END_OF_DATA
        if(c->runner_owned==2) {
            g_media_clips[i]=nullptr;
            media_release_pcm(c);
            if(c->data) free(c->data);
            c->data=nullptr; c->magic=0; free(c);
        }
    }
}

static int media_start_fallback_bgm(const char *asset_name) {
    if(!asset_name || !*asset_name || g_audio_game_bgm_started || g_audio_active_bgm) return -1;
    if(g_audio_fallback_bgm && strcmp(g_audio_fallback_bgm->source_name,asset_name)==0) return 0;
    EmbeddedAssetView a{};
    if(!embedded_asset_find(asset_name,&a) || !a.data || !a.size) {
        printf("[BGM!] asset missing %s\n",asset_name?asset_name:"-");
        return -12;
    }
    DummyMediaClip *c=(DummyMediaClip*)calloc(1,sizeof(DummyMediaClip));
    if(!c) { embedded_asset_release_cache(); return -3; }
    c->magic=MEDIA_MAGIC;
    c->type=0;
    c->volume=100;
    c->sound_id=-1;
    c->callback=0;
    c->runner_owned=1;
    strncpy(c->source_name,asset_name,sizeof(c->source_name)-1);
    c->source_name[sizeof(c->source_name)-1]='\0';
    c->data=(uint8_t*)malloc(a.size);
    if(!c->data) { c->magic=0; free(c); embedded_asset_release_cache(); return -3; }
    memcpy(c->data,a.data,a.size);
    c->data_len=a.size;
    c->data_cap=a.size;
    embedded_asset_release_cache();
    media_register_clip(c);
    const int rc=media_decode(c);
    if(rc!=0 || !c->pcm || !c->pcm_samples) {
        printf("[BGM!] decode %s rc=%d\n",asset_name,rc);
        media_unregister_clip(c);
        media_release_pcm(c);
        free(c->data); c->magic=0; free(c);
        return rc?rc:-1;
    }
    g_audio_last_peak=media_normalize_for_ds(c);
    if(!g_audio_last_peak) {
        printf("[BGM!] silent %s\n",asset_name);
        media_unregister_clip(c);
        media_release_pcm(c);
        free(c->data); c->magic=0; free(c);
        return -4;
    }
    soundEnable();
    soundSetMixerVolume((g_media_profile_volume*127)/100);
    DC_FlushRange(c->pcm,c->pcm_samples*sizeof(int16_t));
    c->repeat=1; c->end_notified=0; c->end_due_ms=0;
    c->sound_id=soundPlaySample(c->pcm,SoundFormat_16Bit,c->pcm_samples*sizeof(int16_t),
                                (u16)c->sample_rate,110,64,true,0);
    c->playing=(c->sound_id>=0)?2:0;
    if(c->sound_id<0) {
        printf("[BGM!] no channel %s\n",asset_name);
        media_unregister_clip(c);
        media_release_pcm(c);
        free(c->data); c->magic=0; free(c);
        return -3;
    }
    soundSetVolume(c->sound_id,110);
    soundSetPan(c->sound_id,64);
    g_audio_fallback_bgm=c;
    g_audio_fallback_bgm_count++;
    g_audio_last_channel=c->sound_id;
    printf("[BGM] fallback %s ch=%d %luHz n=%lu\n",asset_name,c->sound_id,
           (unsigned long)c->sample_rate,(unsigned long)c->pcm_samples);
    return 0;
}

static SfxCacheEntry *sfx_cache_find(const char *name) {
    if(!name) return nullptr;
    for(int i=0;i<SFX_CACHE_SLOTS;i++) {
        if(g_sfx_cache[i].pcm && strcmp(g_sfx_cache[i].name,name)==0) return &g_sfx_cache[i];
    }
    return nullptr;
}

static SfxCacheEntry *sfx_cache_decode(const char *asset_name) {
    if(!asset_name || !*asset_name) return nullptr;
    if(SfxCacheEntry *hit=sfx_cache_find(asset_name)) { g_sfx_cache_hits++; return hit; }
    int free_slot=-1;
    for(int i=0;i<SFX_CACHE_SLOTS;i++) if(!g_sfx_cache[i].pcm) { free_slot=i; break; }
    if(free_slot<0) return nullptr; // keep cached PCM alive; never evict while ARM7 may still read it

    EmbeddedAssetView a{};
    if(!embedded_asset_find(asset_name,&a) || !a.data || !a.size) return nullptr;
    DummyMediaClip tmp{};
    tmp.magic=MEDIA_MAGIC; tmp.type=0; tmp.volume=100; tmp.sound_id=-1;
    strncpy(tmp.source_name,asset_name,sizeof(tmp.source_name)-1);
    tmp.source_name[sizeof(tmp.source_name)-1]='\0';
    tmp.data=(uint8_t*)malloc(a.size);
    if(!tmp.data) { embedded_asset_release_cache(); return nullptr; }
    memcpy(tmp.data,a.data,a.size); tmp.data_len=a.size; tmp.data_cap=a.size;
    embedded_asset_release_cache();

    const int rc=media_decode(&tmp);
    free(tmp.data); tmp.data=nullptr;
    if(rc!=0 || !tmp.pcm || !tmp.pcm_samples || !tmp.sample_rate) {
        media_release_pcm(&tmp);
        return nullptr;
    }
    if(!media_normalize_for_ds(&tmp)) { media_release_pcm(&tmp); return nullptr; }

    SfxCacheEntry &e=g_sfx_cache[free_slot];
    memset(&e,0,sizeof(e));
    strncpy(e.name,asset_name,sizeof(e.name)-1);
    e.pcm=tmp.pcm; e.samples=tmp.pcm_samples; e.rate=tmp.sample_rate;
    tmp.pcm=nullptr; tmp.pcm_samples=0;
    DC_FlushRange(e.pcm,e.samples*sizeof(int16_t));
    g_sfx_cache_misses++;
    printf("[SFX] cached %s %luHz n=%lu\n",e.name,(unsigned long)e.rate,(unsigned long)e.samples);
    return &e;
}

static int media_start_fallback_sfx(const char *asset_name) {
    if(!asset_name || !*asset_name) return -1;
    SfxCacheEntry *e=sfx_cache_decode(asset_name);
    if(!e || !e->pcm || !e->samples || !e->rate) return -4;

    const uint64_t now=real_monotonic_ms();
    const uint64_t dur_ms=((uint64_t)e->samples*1000ULL)/e->rate;
    // Same effect can be requested several times by one logical action.  Do not
    // stack identical channels while the audible attack portion is still active.
    uint64_t guard=dur_ms;
    if(guard>220) guard=220;
    if(guard<80) guard=80;
    if(now<e->busy_until_ms || (e->last_play_ms && now-e->last_play_ms<guard)) return 0;

    soundEnable();
    soundSetMixerVolume((g_media_profile_volume*127)/100);
    const int ch=soundPlaySample(e->pcm,SoundFormat_16Bit,e->samples*sizeof(int16_t),
                                 (u16)e->rate,118,64,false,0);
    if(ch<0) return -3;
    soundSetVolume(ch,118); soundSetPan(ch,64);
    e->last_play_ms=now;
    e->busy_until_ms=now+guard;
    g_audio_fallback_sfx_count++;
    g_audio_last_channel=ch;
    if(g_audio_fallback_sfx_count<=12)
        printf("[SFX] play %s ch=%d cache=%lu/%lu\n",asset_name,ch,
               (unsigned long)g_sfx_cache_hits,(unsigned long)g_sfx_cache_misses);
    return 0;
}

static void media_poll_sfx_fallback(void) {
    if(!g_audio_sfx_pending || !g_audio_last_sfx_asset[0]) return;
    const uint64_t now=real_monotonic_ms();
    // Give Zenonia's own Player.play path enough time to win before recovery.
    if(now-g_audio_sfx_seen_ms < 70) return;
    g_audio_sfx_pending=false;
    if(g_audio_play_count!=g_audio_sfx_play_snapshot) return;
    // De-bounce repeated DB/resource probes of the same effect.
    if(now-g_audio_last_sfx_fire_ms < 70) return;
    g_audio_last_sfx_fire_ms=now;
    char path[32];
    strncpy(path,g_audio_last_sfx_asset,sizeof(path)-1); path[sizeof(path)-1]='\0';
    (void)media_start_fallback_sfx(path);
}

static void media_poll_bgm_fallback(void) {
    const uint64_t now=real_monotonic_ms();

    // If Zenonia reaches its native Play path, always give the game ownership.
    if(g_audio_game_bgm_started) {
        if(g_audio_fallback_bgm) media_stop_fallback_bgm();
        g_audio_bgm_pending=false;
        return;
    }

    // v026: a fallback track is never allowed to become a global soundtrack.
    // When the game requests a different 1xx.mmf, stop the old fallback and
    // replace it with the newly requested track after a short debounce.
    if(g_audio_fallback_bgm) {
        if(g_audio_bgm_pending && g_audio_last_bgm_asset[0] &&
           strcmp(g_audio_fallback_bgm->source_name,g_audio_last_bgm_asset)!=0 &&
           now-g_audio_bgm_seen_ms>=300) {
            char path[32];
            strncpy(path,g_audio_last_bgm_asset,sizeof(path)-1);
            path[sizeof(path)-1]='\0';
            g_audio_bgm_pending=false;
            media_stop_fallback_bgm();
            (void)media_start_fallback_bgm(path);
        } else if(g_audio_bgm_pending &&
                  strcmp(g_audio_fallback_bgm->source_name,g_audio_last_bgm_asset)==0) {
            g_audio_bgm_pending=false;
        }
        return;
    }

    // Only recover an actual BGM resource requested by Zenonia 2. The old v025
    // hard-coded sound/100.mmf five-second fallback is deliberately removed;
    // that caused one song to loop through the entire game.
    if(g_audio_bgm_pending && g_audio_last_bgm_asset[0] && now-g_audio_bgm_seen_ms>=300) {
        char path[32];
        strncpy(path,g_audio_last_bgm_asset,sizeof(path)-1);
        path[sizeof(path)-1]='\0';
        g_audio_bgm_pending=false;
        (void)media_start_fallback_bgm(path);
    }
}

extern "C" int media_noop4(uint32_t,uint32_t,uint32_t,uint32_t) { return 0; }

static void call_event(int type,int key) {
    if(!g_clet.started || !g_clet.callbacks[5]) return;
    typedef void (*Fn)(int,int,int);
    Fn f=(Fn)(uintptr_t)g_clet.callbacks[5];
    f(type,key,0);
}

static void paint_region(int x,int y,int w,int h) {
    if(!g_clet.started || !g_clet.callbacks[4] || g_in_paint) return;
    BootCostScope profile(ZenBootProfile::Paint);
    g_in_paint=true;
    g_minimap_valid=false; // No stale map in menus or after native early return.
    g_rv_map_self=0;
    typedef void (*Fn)(int,int,int,int);
    Fn f=(Fn)(uintptr_t)g_clet.callbacks[4];
    f(x,y,w,h);
    wipi_end_native_hud();
    g_in_paint=false;
}

static void service_repaint(void) {
    if(!g_repaint_pending || g_in_paint) return;
    if(!g_repaint_gate.allow((uint32_t)real_monotonic_ms(),
          g_trace.timer_fire,REG_KEYINPUT,g_log_visible)) return;
    g_repaint_pending=false;
    if(g_trace_repaint_once) printf("[RPT] paint\n");
    // Full screen is deliberate for now: it is safer than guessing whether a
    // handset implementation interprets repaint coordinates as inclusive.
    paint_region(0,0,PHONE_W,PHONE_H);
    g_trace_repaint_once=false;
}

static void process_timers(void) {
    uint64_t t=timer_now_ms();
    for(int i=0;i<TIMER_SLOTS;i++) {
        if(g_timers[i].armed && g_timers[i].callback && t>=g_timers[i].due_ms &&
           real_monotonic_ms()>=g_timers[i].next_fire_real_ms) {
            auto cb=g_timers[i].callback; void *timer=g_timers[i].timer; void *param=g_timers[i].param;
            const uintptr_t cbaddr=(uintptr_t)cb;
            if((cbaddr&~1u)<0x02000000u || (cbaddr&~1u)>=0x04000000u) {
                printf("[TMR!] BAD cb=%p s=%d\n",(void*)cb,i);
                g_timers[i].armed=false;
                continue;
            }
            g_timers[i].armed=false; g_trace.timer_fire++;
            BootCostScope profile(ZenBootProfile::Timer);
#if ZEN_CONSERVATIVE_CALLBACK_CACHE
            DC_FlushAll(); IC_InvalidateAll();
#endif
            // Trace the first timer callback as a critical section.  Previous
            // builds had the g_trace_next_timer hook but never enabled it, so
            // unresolved WIPI imports made from inside the timer were invisible.
            // Keep the line deliberately short: the DS text console is only
            // about 32 columns wide and long lines hide the callback address.
            g_trace_next_timer=true;
            if(g_unknown_trace_budget<24) g_unknown_trace_budget=24;
            g_known_trace_budget=32;
            if(g_timer_trace_budget) {
                printf("[T>] s=%d cb=%08lx\n",i,(unsigned long)cbaddr);
                uint32_t oldpc=0;
                const uintptr_t clean=cbaddr&~(uintptr_t)1u;
                if(g_guest_text_new && clean>=g_guest_text_new && clean<g_guest_text_new+g_guest_text_size)
                    oldpc=g_guest_text_old+(uint32_t)(clean-g_guest_text_new)|(uint32_t)(cbaddr&1u);
                printf("[TPC] old=%08lx p=%08lx\n",
                       (unsigned long)oldpc,(unsigned long)(uintptr_t)param);
                if(clean>=0x02000000u && clean+8u<0x04000000u) {
                    const uint16_t *hw=(const uint16_t*)clean;
                    printf("[TC0] %04x %04x %04x %04x\n",hw[0],hw[1],hw[2],hw[3]);
                }
                g_timer_trace_budget--;
            }
            // Charge the interval before entering guest code. SetTimer may
            // rearm this slot immediately; it must not erase the pacing deadline.
            g_timers[i].next_fire_real_ms=real_monotonic_ms()+GUEST_TIMER_MIN_INTERVAL_MS;
            cb(timer,param);
            if(g_timer_trace_budget) {
                printf("[T<] s=%d ret rp=%lu fl=%lu im=%lu\n",i,
                       (unsigned long)g_trace.repaint,
                       (unsigned long)g_trace.flush,
                       (unsigned long)g_trace.draw_image);
                g_timer_trace_budget--;
            }
            g_trace_next_timer=false;
            g_known_trace_budget=0;
            static bool dumped_after_first_timer=false;
            if(!dumped_after_first_timer) {
                dumped_after_first_timer=true;
                wipi_debug_dump("after timer");
            }
        }
    }
}
}

uint32_t zen_boot_clock_ms(void) { return (uint32_t)real_monotonic_ms(); }

// Public storage entry points must have external linkage because main.cpp calls
// them from a different translation unit.  The implementation itself stays in
// the anonymous namespace so all DB/FAT state remains private to this file.
void wipi_set_guest_text_mapping(uint32_t old_base,const void *new_base,uint32_t size) {
    g_guest_text_old=old_base;
    g_guest_text_new=(uintptr_t)new_base;
    g_guest_text_size=size;
    printf("[MAP] text %08lx -> %08lx sz=%08lx\n",
           (unsigned long)old_base,(unsigned long)g_guest_text_new,(unsigned long)size);
}

void wipi_init_storage(void) {
    storage_init_internal();
}

void wipi_flush_storage(void) {
    storage_flush_internal();
}

void wipi_note_sound_request(int id,int arg2,int flag) {
    audio_note_sound_request_internal(id,arg2,flag);
}

static void runner_release_all_logical(void);

void wipi_init_video(PrintConsole *log_console) {
    lcdMainOnTop();
    // Main/top: 256x192 presentation bitmap for the scaled guest framebuffer.
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    g_main_bg=bgInit(3,BgType_Bmp16,BgSize_B16_256x256,0,0);
    g_ds_fb=(uint16_t*)bgGetGfxPtr(g_main_bg);
    memset(g_ds_fb,0,256*256*2);

    // Sub/bottom: full 16-bit bitmap in VRAM C so colors match the top LCD.
    // VRAM H cannot stay mapped as SUB_BG at the same time: it aliases the
    // beginning of the SUB BG address space and causes the horizontal garbage
    // band/flicker seen in v026. Initialize the settings console in VRAM H, then
    // unmap H during gameplay. We swap C/H only while the settings screen is open.
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_LCD);
    vramSetBankH(VRAM_H_SUB_BG);

    g_log_console=log_console;
    if(g_log_console) {
        consoleInit(g_log_console,1,BgType_Text4bpp,BgSize_T_256x256,8,0,false,true);
        g_log_bg=g_log_console->bgId;
        bgSetPriority(g_log_bg,0);
        bgHide(g_log_bg);
        consoleSelect(g_log_console);
        consoleClear();
    }

    // Gameplay mapping: VRAM C owns the whole SUB bitmap; VRAM H is detached.
    vramSetBankH(VRAM_H_LCD);
    vramSetBankC(VRAM_C_SUB_BG);
    if(g_log_console) g_log_console->fontBgMap=(uint16_t*)ZenDS::ConsoleHiddenMap;
    g_sub_bg=bgInitSub(3,BgType_Bmp16,BgSize_B16_256x256,0,0);
    g_sub_fb=(uint16_t*)bgGetGfxPtr(g_sub_bg);
    memset(g_sub_fb,0,256*256*2);
    g_sub_comp=(uint16_t*)memalign(32,256*192*2);
    if(g_sub_comp) memset(g_sub_comp,0,256*192*2);
    bgSetPriority(g_sub_bg,3);

    // Reserve a simple text palette for the hidden settings/log console.
    BG_PALETTE_SUB[0]=RGB15(0,0,0);
    BG_PALETTE_SUB[15]=RGB15(31,31,31);

    g_phone_fb=(uint16_t*)memalign(32,PHONE_W*PHONE_H*2);
    if(g_phone_fb) memset(g_phone_fb,0,PHONE_W*PHONE_H*2);
    g_dialogue_snapshot=(uint16_t*)memalign(32,PHONE_W*UI_DIALOG_CAPTURE_H*2);
    if(g_dialogue_snapshot) memset(g_dialogue_snapshot,0,PHONE_W*UI_DIALOG_CAPTURE_H*2);
    g_thunk_pool=(uint32_t*)memalign(32,UNKNOWN_THUNK_SLOTS*5*sizeof(uint32_t));
    if(g_thunk_pool) memset(g_thunk_pool,0,UNKNOWN_THUNK_SLOTS*5*sizeof(uint32_t));
    g_known_trace_pool=(uint32_t*)memalign(32,KNOWN_TRACE_THUNK_SLOTS*10*sizeof(uint32_t));
    if(g_known_trace_pool) memset(g_known_trace_pool,0,KNOWN_TRACE_THUNK_SLOTS*10*sizeof(uint32_t));
    g_known_trace_count=0;
    g_known_trace_budget=0;
    g_screen_fb.width=PHONE_W; g_screen_fb.height=PHONE_H; g_screen_fb.bpl=PHONE_W*2;
    g_screen_fb.bpp=16; g_screen_fb.buf=(uint32_t)(uintptr_t)g_phone_fb;
    g_screen_view.ptr_backing=(uint32_t)(uintptr_t)&g_screen_fb;
    g_screen_view.x=0; g_screen_view.y=0; g_screen_view.width=PHONE_W; g_screen_view.height=PHONE_H;
    g_screen_wrapper.owned_image=0;
    g_screen_wrapper.ptr_graphics=(uint32_t)(uintptr_t)&g_screen_view;
    g_screen_wrapper.ptr_image=(uint32_t)(uintptr_t)&g_screen_fb;
    g_screen_wrapper.screen_kind=0;

    runner_ui_defaults();
    runner_ui_log_push_line("WIPI Runner settings ready");

    // cpuStartTiming() ensures the Calico tick system is initialized in current
    // libnds. The WIPI scheduler itself uses tickGetCount()/TICK_FREQ.
    cpuStartTiming(0);
    g_tick_base=tickGetCount();
    g_clock_started=true;
    g_timer_clock_ms=0;
    g_timer_wait_next_ms=0;
    g_loop_report_next_ms=0;
    g_loop_count=0;
    g_timer_trace_budget=12; // first timer lifecycle is critical for Ragnarok boot
    printf("[CLK] tick base=%lu freq=%lu\n",
           (unsigned long)g_tick_base,(unsigned long)TICK_FREQ);
}

static inline uint16_t rgb565_to_ds15(uint16_t p) {
    // WIPI exposes the handset framebuffer as RGB565 (RRRRRGGGGGGBBBBB).
    // Nintendo DS bitmap backgrounds use RGB15 (0BBBBBGGGGGRRRRR) with
    // bit 15 set for an opaque pixel. Convert channels instead of copying
    // the 16-bit value directly, otherwise red/blue are swapped.
    const uint16_t r=(p>>11)&31u;
    const uint16_t g=(p>>6)&31u; // 6-bit WIPI green -> 5-bit DS green
    const uint16_t b=p&31u;
    return (uint16_t)(BIT(15) | r | (g<<5) | (b<<10));
}

static inline uint8_t rgb565_to_sub8(uint16_t p) {
    const int r5=(p>>11)&31;
    const int g6=(p>>5)&63;
    const int b5=p&31;
    const int r=(r5*5+15)/31;
    const int g=(g6*7+31)/63;
    const int b=(b5*4+15)/31;
    return (uint8_t)(16 + r*40 + g*5 + b);
}

static void sub_put_to(uint16_t *fb,int x,int y,uint16_t c) {
    if(!fb || x<0 || y<0 || x>=DS_W || y>=DS_H) return;
    fb[y*256+x]=c;
}

static void sub_put(int x,int y,uint16_t c) {
    sub_put_to(g_sub_fb,x,y,c);
}

static void sub_rect_to(uint16_t *fb,int x,int y,int w,int h,uint16_t c) {
    if(!fb || w<=0 || h<=0) return;
    for(int yy=0;yy<h;yy++) for(int xx=0;xx<w;xx++) sub_put_to(fb,x+xx,y+yy,c);
}

static void sub_rect(int x,int y,int w,int h,uint16_t c) {
    sub_rect_to(g_sub_fb,x,y,w,h,c);
}

static void draw_3x5_glyph_to(uint16_t *fb,int x,int y,const uint8_t rows[5],int scale,uint16_t c) {
    for(int yy=0;yy<5;yy++) for(int xx=0;xx<3;xx++) {
        if(rows[yy]&(1u<<(2-xx))) sub_rect_to(fb,x+xx*scale,y+yy*scale,scale,scale,c);
    }
}

static void draw_log_button_to(uint16_t *fb) {
    if(fb && !g_log_visible) ZenMapUI::gear(fb);
}

static inline int rgb565_luma(uint16_t p) {
    const int r=((p>>11)&31)*255/31;
    const int g=((p>>5)&63)*255/63;
    const int b=(p&31)*255/31;
    return (r*3 + g*6 + b) / 10;
}

// The phone renderer is one already-composited framebuffer, so there is no
// native UI layer we can simply move to the touch screen.  v017/v018 copied a
// large y=176..319 slice to the lower LCD; that is exactly why normal map pixels
// appeared there.  v019 keeps the lower LCD empty and only enables the dialogue
// window when the lower source area actually looks like a broad UI panel.
// Hysteresis keeps a dialogue from blinking while letters/portraits animate.
static bool detect_dialogue_panel(void) {
    if(!g_phone_fb) return false;
    int dark=0, samples=0, strong_rows=0;
    for(int y=176; y<PHONE_H; y+=3) {
        int row_dark=0, row_samples=0;
        for(int x=6; x<PHONE_W-6; x+=3) {
            const int l=rgb565_luma(g_phone_fb[y*PHONE_W+x]);
            if(l<92) { dark++; row_dark++; }
            samples++; row_samples++;
        }
        if(row_samples && row_dark*100 >= row_samples*52) strong_rows++;
    }
    // Require both a sizeable dark area and several long dark rows.  Ordinary
    // grass/road tiles have many dark pixels but rarely form a wide flat panel.
    return samples && dark*100 >= samples*25 && strong_rows>=4;
}

static bool detect_info_page(void) {
    if(!g_phone_fb) return false;
    int bright=0, red=0, samples=0;
    for(int y=24; y<PHONE_H-24; y+=4) {
        for(int x=16; x<PHONE_W-16; x+=4) {
            const uint16_t p=g_phone_fb[y*PHONE_W+x];
            const int r=(p>>11)&31;
            const int g=(p>>5)&63;
            const int b=p&31;
            const int l=rgb565_luma(p);
            if(l>220) bright++;
            if(r>20 && g<20 && b<20) red++;
            samples++;
        }
    }
    // White instruction/help pages in Zenonia 2 use a very bright background
    // with dense red text. Detect that so the bottom screen does not become an
    // empty white square.
    return samples && bright*100 >= samples*55 && red*100 >= samples*2;
}

static void copy_phone_rect_to_ds_buf(uint16_t *dst,int sx,int sy,int w,int h,int dx,int dy) {
    if(!g_phone_fb || !dst) return;
    for(int yy=0; yy<h; yy++) {
        const int py=sy+yy, ty=dy+yy;
        if(py<0 || py>=PHONE_H || ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<w; xx++) {
            const int px=sx+xx, tx=dx+xx;
            if(px<0 || px>=PHONE_W || tx<0 || tx>=DS_W) continue;
            dst[ty*256+tx]=rgb565_to_ds15(g_phone_fb[py*PHONE_W+px]);
        }
    }
}

static void copy_phone_rect_to_main(int sx,int sy,int w,int h,int dx,int dy) {
    copy_phone_rect_to_ds_buf(g_ds_fb,sx,sy,w,h,dx,dy);
}

static void copy_phone_rect_to_sub_buf(uint16_t *dst,int sx,int sy,int w,int h,int dx,int dy) {
    copy_phone_rect_to_ds_buf(dst,sx,sy,w,h,dx,dy);
}

static void copy_phone_rect_to_sub(int sx,int sy,int w,int h,int dx,int dy) {
    copy_phone_rect_to_sub_buf(g_sub_fb,sx,sy,w,h,dx,dy);
}

static void copy_phone_rect_scaled_to_sub_buf(uint16_t *dst,int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh) {
    if(!g_phone_fb || !dst || sw<=0 || sh<=0 || dw<=0 || dh<=0) return;
    for(int yy=0; yy<dh; yy++) {
        const int py=sy + (yy*sh)/dh;
        const int ty=dy+yy;
        if(py<0 || py>=PHONE_H || ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<dw; xx++) {
            const int px=sx + (xx*sw)/dw;
            const int tx=dx+xx;
            if(px<0 || px>=PHONE_W || tx<0 || tx>=DS_W) continue;
            dst[ty*256+tx]=rgb565_to_ds15(g_phone_fb[py*PHONE_W+px]);
        }
    }
}

static void copy_snapshot_rect_to_sub(int w,int h,int dx,int dy) {
    if(!g_dialogue_snapshot || !g_sub_comp) return;
    for(int yy=0; yy<h; yy++) {
        const int ty=dy+yy;
        if(ty<0 || ty>=DS_H) continue;
        for(int xx=0; xx<w; xx++) {
            const int tx=dx+xx;
            if(tx<0 || tx>=DS_W) continue;
            g_sub_comp[ty*256+tx]=rgb565_to_ds15(g_dialogue_snapshot[yy*PHONE_W+xx]);
        }
    }
}

static void snapshot_dialogue(int src_y) {
    if(!g_phone_fb || !g_dialogue_snapshot) return;
    if(src_y<0) src_y=0;
    if(src_y>PHONE_H-UI_DIALOG_CAPTURE_H) src_y=PHONE_H-UI_DIALOG_CAPTURE_H;
    g_dialogue_capture_y=src_y;
    for(int yy=0; yy<UI_DIALOG_CAPTURE_H; yy++) {
        memcpy(g_dialogue_snapshot + yy*PHONE_W,
               g_phone_fb + (src_y+yy)*PHONE_W,
               PHONE_W*2);
    }
    g_dialogue_snapshot_valid=true;
}

// v042: scope guards geometry changes only. Draw the native HUD straight
// into TOP with gameplay; no SUB target, extra allocation or pixel compositing.
bool wipi_begin_native_hud(void) {
    if(g_native_hud_active || !g_phone_fb) return false;
    g_native_hud_active=true;
    return true;
}
void wipi_end_native_hud(void) {
    g_native_hud_active=false;
}

void wipi_invalidate_minimap_cache(void) { g_minimap_cache.invalidate(); }
bool wipi_try_cached_minimap(uint32_t self,uint32_t map,int w,int h,uint32_t *result) {
    if(!g_minimap_bottom){g_minimap_cache.invalidate();return false;}
    const uint32_t now=(uint32_t)real_monotonic_ms();
    if(g_minimap_cache.reuse(now,self,map,(uint32_t)w,(uint32_t)h)) {
        g_minimap_valid=true;
        if(result)*result=g_minimap_cache.result;
        return true;
    }
    g_minimap_cache.begin(now,self,map,(uint32_t)w,(uint32_t)h);
    return false;
}
void wipi_store_minimap_cache(uint32_t result) {
    g_minimap_cache.finish(result,g_minimap_valid);
}

bool wipi_prepare_minimap(void) {
    if(!g_minimap_fb) g_minimap_fb=(uint16_t*)memalign(32,PHONE_W*PHONE_H*2);
    g_minimap_ready=g_minimap_fb!=nullptr;
    return g_minimap_ready;
}
bool wipi_begin_minimap(int w,int h) {
    if(!g_minimap_ready || !g_minimap_bottom || g_minimap_capture || !g_phone_fb ||
       w<1 || w>252 || h<1 || h>124) return false;
    g_minimap_w=w+4;g_minimap_h=h+4;
    memset(g_minimap_fb,0,PHONE_W*PHONE_H*2);
    g_minimap_capture=true;
    g_screen_fb.buf=(uint32_t)(uintptr_t)g_minimap_fb;
    return true;
}
bool wipi_minimap_has_pixels(void) {
    if(!g_minimap_capture)return false;
    for(int y=0;y<g_minimap_h;y++)for(int x=0;x<g_minimap_w;x++)
        if(g_minimap_fb[y*PHONE_W+x])return true;
    return false;
}
void wipi_end_minimap(void) {
    if(!g_minimap_capture)return;
    g_minimap_valid=wipi_minimap_has_pixels();
    g_screen_fb.buf=(uint32_t)(uintptr_t)g_phone_fb;
    g_minimap_capture=false;
}
void wipi_toggle_minimap(void) {
    if(!g_minimap_ready)return;
    g_minimap_bottom=!g_minimap_bottom;g_minimap_valid=false;
    g_minimap_cache.invalidate();g_minimap_panel.invalidate();
    g_sub_static_valid=false;
    g_repaint_pending=true; // Redraw native scene; never erase a rectangle from TOP.
}

void wipi_note_ragnarok_map(uint32_t self) {
    if(self>=0x02000000u && self<=0x02400000u-0x3330u) g_rv_map_self=self;
}
static void rv_draw_touch_panel(uint16_t *dst) {
    if(!g_rv_map_self) return;
    const uint8_t *map=(const uint8_t*)(uintptr_t)g_rv_map_self;
    const int w=map[0x32dd],h=map[0x32de];
    if(w<1 || w>51 || h<1 || h>51) return;
    // Native map cells are column-major, 51 signed 16-bit tiles per column.
    // Use the same atlas as the map renderer for a complete terrain thumbnail.
    const uint16_t *cells=(const uint16_t*)(map+16);
    RvImageAllocation *atlas=rv_image(*(const uint32_t*)(map+4));
    uint16_t *pixels=atlas?fb_pixels16(&atlas->backing):nullptr;
    const int aw=atlas?(int)atlas->backing.width:0;
    const int ah=atlas?(int)atlas->backing.height:0;
    int dw=216,dh=dw*h/w;
    if(dh>120) {dh=120;dw=dh*w/h;}
    const int dx=(DS_W-dw)/2,dy=12+(120-dh)/2;
    sub_rect_to(dst,dx-1,dy-1,dw+2,dh+2,RGB15(18,16,25)|BIT(15));
    for(int y=0;y<dh;y++) for(int x=0;x<dw;x++) {
        const int wx=x*w*18/dw,wy=y*h*18/dh;
        const unsigned tile=cells[(wx/18)*51+wy/18];
        uint16_t color=RGB15(8,10,12)|BIT(15);
        const int sx=(tile%10)*18+wx%18,sy=(tile/10)*18+wy%18;
        if(pixels && sx<aw && sy<ah) color=rgb565_to_ds15(pixels[sy*(atlas->backing.bpl/2)+sx]);
        else if(tile<256) color=RGB15(10+(tile%7),12+(tile%9),10+(tile%5))|BIT(15);
        dst[(dy+y)*DS_W+dx+x]=color;
    }
    // Show the actual camera window on the full map.
    const int cx=*(const int16_t*)(map+0x3328),cy=*(const int16_t*)(map+0x332a);
    int l=cx*dw/(w*18),t=cy*dh/(h*18),rw=PHONE_W*dw/(w*18),rh=DS_H*dh/(h*18);
    if(l<0)l=0;if(t<0)t=0;if(rw<2)rw=2;if(rh<2)rh=2;
    if(l+rw>dw)rw=dw-l;if(t+rh>dh)rh=dh-t;
    const uint16_t marker=RGB15(31,28,8)|BIT(15);
    if(rw>0 && rh>0) {
        sub_rect_to(dst,dx+l,dy+t,rw,1,marker);sub_rect_to(dst,dx+l,dy+t+rh-1,rw,1,marker);
        sub_rect_to(dst,dx+l,dy+t,1,rh,marker);sub_rect_to(dst,dx+l+rw-1,dy+t,1,rh,marker);
    }
    static const uint8_t labels[6][5]={
        {2,6,2,2,7},{6,1,3,1,6},{7,1,2,2,2},
        {2,5,3,1,6},{7,5,5,5,7},{5,7,5,7,5}
    };
    for(int i=0;i<6;i++) {
        const int x=8+i*40;
        const bool pressed=(g_touch_logical&(1u<<(4+RV_QUICK_TARGETS[i])))!=0;
        sub_rect_to(dst,x,144,38,44,RGB15(18,16,25)|BIT(15));
        sub_rect_to(dst,x+1,145,36,42,(pressed?RGB15(14,12,24):RGB15(5,5,10))|BIT(15));
        draw_3x5_glyph_to(dst,x+14,157,labels[i],3,RGB15(31,30,31)|BIT(15));
    }
}

void wipi_present(void) {
    if(!g_phone_fb||!g_ds_fb||!g_sub_fb) return;
    g_present_pending=false;

    // Fit handset width to TOP, keeping guest Y coordinates at native scale.
    // Rows below the 192px LCD remain clipped until the next layout pass.
    constexpr size_t LCD_BYTES=DS_W*DS_H*2u;
    const uint16_t black=(uint16_t)(BIT(15) | RGB15(0,0,0));
    uint16_t *top=g_sub_comp ? g_sub_comp : g_ds_fb;
    for(int y=0;y<DS_H;y++) {
        const uint16_t *src=g_phone_fb+y*PHONE_W;
        for(int x=0;x<DS_W;x++)
            top[y*DS_W+x]=rgb565_to_ds15(src[x*PHONE_W/DS_W]);
    }
    // Compose in RAM before transferring, so scanout cannot see a blank frame.
    if(g_sub_comp) {
        DC_FlushRange(g_sub_comp,LCD_BYTES);
        dmaCopy(g_sub_comp,g_ds_fb,LCD_BYTES);
    }

    // SUB is reserved for the existing settings/log controls.
    if(!g_log_visible) {
        uint16_t *sub=g_sub_comp ? g_sub_comp : g_sub_fb;
        ZenRenderFast::fill16(sub,DS_W*DS_H,black);
        rv_draw_touch_panel(sub);
        draw_log_button_to(sub);
        if(g_sub_comp) {
            DC_FlushRange(g_sub_comp,LCD_BYTES);
            dmaCopy(g_sub_comp,g_sub_fb,LCD_BYTES);
        }
    }
}


// Boot probe is drawn directly into the TOP LCD BG3 bitmap. It does not
// depend on the Ragnarok ELF, WIPI repaint callbacks, or SUB text console.
// If guest code stops the ARM9, the last stage remains readable on screen.
static const char kProbeAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-./ ";
static const uint8_t kProbe3x5[][5] = {
    /* A */ {2,5,7,5,5},
    /* B */ {6,5,6,5,6},
    /* C */ {3,4,4,4,3},
    /* D */ {6,5,5,5,6},
    /* E */ {7,4,6,4,7},
    /* F */ {7,4,6,4,4},
    /* G */ {3,4,5,5,3},
    /* H */ {5,5,7,5,5},
    /* I */ {7,2,2,2,7},
    /* J */ {1,1,1,5,2},
    /* K */ {5,5,6,5,5},
    /* L */ {4,4,4,4,7},
    /* M */ {5,7,7,5,5},
    /* N */ {5,7,7,7,5},
    /* O */ {2,5,5,5,2},
    /* P */ {6,5,6,4,4},
    /* Q */ {2,5,5,7,3},
    /* R */ {6,5,6,5,5},
    /* S */ {3,4,2,1,6},
    /* T */ {7,2,2,2,2},
    /* U */ {5,5,5,5,7},
    /* V */ {5,5,5,5,2},
    /* W */ {5,5,7,7,5},
    /* X */ {5,5,2,5,5},
    /* Y */ {5,5,2,2,2},
    /* Z */ {7,1,2,4,7},
    /* 0 */ {7,5,5,5,7},
    /* 1 */ {2,6,2,2,7},
    /* 2 */ {6,1,7,4,7},
    /* 3 */ {6,1,3,1,6},
    /* 4 */ {5,5,7,1,1},
    /* 5 */ {7,4,6,1,6},
    /* 6 */ {3,4,6,5,2},
    /* 7 */ {7,1,2,2,2},
    /* 8 */ {2,5,2,5,2},
    /* 9 */ {2,5,3,1,6},
    /* : */ {0,2,0,2,0},
    /* - */ {0,0,7,0,0},
    /* . */ {0,0,0,0,2},
    /* / */ {1,1,2,4,4},
    /*   */ {0,0,0,0,0}
};
static void probe_rect(int x,int y,int w,int h,uint16_t color) {
    if(!g_ds_fb) return;
    if(x<0 || y<0 || w<=0 || h<=0 || x+w>DS_W || y+h>DS_H) return;
    for(int yy=y; yy<y+h; yy++)
        for(int xx=x; xx<x+w; xx++) g_ds_fb[yy*256+xx]=color;
}
static void probe_text(int x,int y,const char *str,uint16_t color,int scale) {
    if(!str) return;
    for(; *str && x+4*scale<DS_W; ++str,x+=4*scale) {
        char c=*str;
        if(c>='a' && c<='z') c=(char)(c-'a'+'A');
        const char *f=strchr(kProbeAlphabet,c);
        if(!f) continue;
        const uint8_t *rows=kProbe3x5[(size_t)(f-kProbeAlphabet)];
        for(int ry=0;ry<5;ry++) for(int rx=0;rx<3;rx++)
            if(rows[ry]&(1u<<(2-rx)))
                probe_rect(x+rx*scale,y+ry*scale,scale,scale,color);
    }
}
void wipi_boot_stage(const char *stage) {
    if(!stage) stage="UNKNOWN";
    // Record boot stage in the same ring as WIPI/import messages. When the
    // LOG UI is open this redraws the text console independently of the guest.
    runner_ui_log_push_line(stage);
    g_ui_dirty=true;
    if(g_log_visible) runner_ui_draw();
    if(!g_ds_fb) return;
    const uint16_t bg=(uint16_t)(BIT(15)|RGB15(2,4,8));
    const uint16_t white=(uint16_t)(BIT(15)|RGB15(30,30,30));
    const uint16_t cyan=(uint16_t)(BIT(15)|RGB15(4,28,29));
    const uint16_t bar=(uint16_t)(BIT(15)|RGB15(24,17,5));
    for(int y=0;y<DS_H;y++) for(int x=0;x<DS_W;x++) g_ds_fb[y*256+x]=bg;
    probe_text(12,18,"RAGNAROK VIOLET",white,2);
    probe_text(12,46,"NDS BOOT DIAGNOSTIC",cyan,2);
    probe_text(12,85,stage,white,2);
    probe_text(12,160,"SEE LOG ON BOTTOM",cyan,2);
    int n=0;
    if(stage[0]>='0' && stage[0]<='9') n=stage[0]-'0';
    if(stage[1]>='0' && stage[1]<='9') n=n*10+stage[1]-'0';
    if(n>10) n=10;
    if(n>0) probe_rect(12,122,(n*232)/10,9,bar);
}

void wipi_set_log_visible(bool visible) {
    if(!g_log_console || g_log_bg<0 || g_sub_bg<0) return;
    if(g_log_visible==visible) return;
    if(visible) runner_release_all_logical();
    g_log_visible=visible;
    g_sub_static_valid=false;
    g_minimap_panel.invalidate();
    g_ui_capture_single=-1;
    g_ui_capture_combo=-1;

    // v005 melonDS debug: do not block on VBlank while switching the log view.
    if(visible) {
        bgHide(g_sub_bg);
        vramSetBankC(VRAM_C_LCD);
        vramSetBankH(VRAM_H_SUB_BG);
        g_log_console->fontBgMap=(uint16_t*)ZenDS::ConsoleSubMap;
        bgShow(g_log_bg);
        consoleSelect(g_log_console);
        g_ui_tab=UI_TAB_LOG;
        g_ui_dirty=true;
        runner_ui_log_push_line("settings opened");
        runner_ui_draw();
    } else {
        runner_ui_log_push_line("settings closed");
        bgHide(g_log_bg);
        vramSetBankH(VRAM_H_LCD);
        g_log_console->fontBgMap=(uint16_t*)ZenDS::ConsoleHiddenMap;
        vramSetBankC(VRAM_C_SUB_BG);
        bgShow(g_sub_bg);
        g_ui_prev_logical=0;
        // Rebuild the full bottom frame after remapping C back to SUB_BG.
        wipi_present();
    }
    bgUpdate();
}

void wipi_set_ragnarok_ez_bridge(volatile uint32_t *game_state, void *ui_key_handler) {
    g_rv_game_state=game_state;
    uintptr_t p=(uintptr_t)ui_key_handler;
    if(p) p|=1u; // binary.mod UI handler is Thumb code
    g_rv_ui_key_handler=(RvUiKeyHandler)p;
    g_rv_direct_ez_held=false;
}

bool wipi_log_visible(void) { return g_log_visible; }

void wipi_handle_touch(uint32_t down,int x,int y) {
    if(!(down&KEY_TOUCH)) return;

    // Six synthetic 4..9 buttons and their hit boxes are removed. LOG remains.
    if(!g_log_visible) {
        if(x>=LOG_BTN_X && x<LOG_BTN_X+LOG_BTN_W && y>=LOG_BTN_Y && y<LOG_BTN_Y+LOG_BTN_H) {
            wipi_set_log_visible(true);
            return;
        }
        if(g_rv_map_self && y>=144 && y<188 && x>=8 && x<248) {
            const int slot=(x-8)/40;
            g_touch_logical=1u<<(4+RV_QUICK_TARGETS[slot]);
        }
        return;
    }

    if(g_log_visible) {
        // Top row: SINGLE | COMBO | LOG | CLOSE. No MAP tab is present.
        if(y<UI_TARGET_TOP) {
            if(x<72) g_ui_tab=UI_TAB_SINGLE;
            else if(x<144) g_ui_tab=UI_TAB_COMBO;
            else if(x<200) g_ui_tab=UI_TAB_LOG;
            else { wipi_set_log_visible(false); return; }
            g_ui_capture_single=-1;
            g_ui_capture_combo=-1;
            g_ui_dirty=true;
            runner_ui_draw();
            return;
        }
        // Eleven full-height touch rows from y=16..191, aligned with text.
        if((g_ui_tab==UI_TAB_SINGLE || g_ui_tab==UI_TAB_COMBO) && y>=UI_TARGET_TOP && y<DS_H) {
            int idx=(y-UI_TARGET_TOP)/UI_TARGET_ROW_H;
            if(idx<0) idx=0; if(idx>=UI_TARGET_COUNT) idx=UI_TARGET_COUNT-1;
            if(g_ui_tab==UI_TAB_SINGLE) {
                g_ui_selected_single=idx;
                g_ui_capture_single=idx;
                g_ui_capture_combo=-1;
            } else {
                g_ui_selected_combo=idx;
                g_ui_capture_combo=idx;
                g_ui_capture_single=-1;
            }
            g_ui_dirty=true;
            runner_ui_draw();
        }
        return;
    }

}

void wipi_debug_dump(const char *tag) {
    uint32_t nz=0;
    if(g_phone_fb) {
        for(int i=0;i<PHONE_W*PHONE_H;i++) if(g_phone_fb[i]) nz++;
    }
    printf("[TRACE] %s\n", tag?tag:"-");
    printf("[DBS] open=%lu fail=%lu read=%lu bytes=%lu\n",
           (unsigned long)g_trace.db_opens,(unsigned long)g_trace.db_open_fail,
           (unsigned long)g_trace.db_reads,(unsigned long)g_trace.db_read_bytes);
    printf("[GFX] fb=%lu ctx=%lu/%lu rect=%lu/%lu img=%lu str=%lu\n",
           (unsigned long)g_trace.get_fb,(unsigned long)g_trace.init_ctx,(unsigned long)g_trace.set_ctx,
           (unsigned long)g_trace.draw_rect,(unsigned long)g_trace.fill_rect,
           (unsigned long)g_trace.draw_image,(unsigned long)g_trace.draw_string);
    printf("[GFX] px=%lu create=%lu flush=%lu repaint=%lu nz=%lu\n",
           (unsigned long)g_trace.put_pixel,(unsigned long)g_trace.create_image,
           (unsigned long)g_trace.flush,(unsigned long)g_trace.repaint,(unsigned long)nz);
    printf("[GFX2] off=%lu/%lu copy=%lu/%lu rgb=%lu/%lu\n",
           (unsigned long)g_trace.offscreen_create,(unsigned long)g_trace.offscreen_destroy,
           (unsigned long)g_trace.copy_fb,(unsigned long)g_trace.copy_area,
           (unsigned long)g_trace.get_rgb,(unsigned long)g_trace.set_rgb);
    unsigned armed=0; for(int i=0;i<TIMER_SLOTS;i++) if(g_timers[i].armed) armed++;
    printf("[TSTAT] set=%lu fire=%lu armed=%u\n",(unsigned long)g_trace.timer_set,(unsigned long)g_trace.timer_fire,armed);
}

void wipi_poll(void) {
    // v009 melonDS debug: nonblocking main loop, timed by Calico's monotonic
    // system tick counter rather than VBlank or accumulated cpuGetTiming deltas.
    g_loop_count++;
    process_timers();
    media_poll_end_events();
    media_poll_bgm_fallback();
    media_poll_sfx_fallback();
    // A timer/event may have requested a repaint. Run it only after that guest
    // callback has returned, then present the finished frame once.
    service_repaint();
    if(g_present_pending) {
        g_present_pending=false;
        wipi_present();
    }
    if(g_log_visible && g_ui_dirty) {
        const uint64_t now=real_monotonic_ms();
        // 10 Hz is fast enough for diagnostics but avoids console redraw flicker.
        if(now-g_log_last_draw_ms>=100u) {
            g_log_last_draw_ms=now;
            runner_ui_draw();
        }
    }
}

uint32_t wipi_get_import_table(uint32_t table) { return table; }

void *wipi_get_import_function(uint32_t table, uint32_t index) {
    void *fn=nullptr;
    if(table==0x1fb) {
        switch(index) {
            case 0x03: fn=(void*)clet_register; break;
            case 0x32: fn=(void*)fb_ptr; break;
            case 0x33: fn=(void*)fb_w; break;
            case 0x34: fn=(void*)fb_h; break;
            case 0x35: fn=(void*)fb_bpl; break;
            case 0x36: fn=(void*)fb_bpp; break;
            case 0x64: fn=(void*)knl_printk; break;
            case 0x65: fn=(void*)knl_sprintk; break;
            case 0x68: fn=(void*)knl_unk68; break;
            case 0x6a: fn=(void*)knl_unk6a; break;
            case 0x6b: fn=(void*)knl_exit; break;
            case 0x6f: fn=(void*)knl_get_program_name; break;
            case 0x75: fn=(void*)knl_alloc; break;
            case 0x76: fn=(void*)knl_calloc; break;
            case 0x77: fn=(void*)knl_free; break;
            case 0x78: fn=(void*)knl_total_mem; break;
            case 0x79: fn=(void*)knl_free_mem; break;
            case 0x7a: fn=(void*)knl_def_timer; break;
            case 0x7b: fn=(void*)knl_set_timer; break;
            case 0x7c: fn=(void*)knl_unset_timer; break;
            case 0x7d: fn=(void*)knl_current_time; break;
            case 0x7e: fn=(void*)knl_get_system_property; break;
            case 0x7f: fn=(void*)knl_set_system_property; break;
            case 0x80: fn=(void*)knl_get_resource_id; break;
            case 0x81: fn=(void*)knl_get_resource; break;
            case 0xc8: fn=(void*)gfx_get_image_property; break;
            case 0xc9: fn=(void*)gfx_get_image_framebuffer; break;
            case 0xca: fn=(void*)gfx_get_screen_fb; break;
            case 0xcb: fn=(void*)gfx_destroy_offscreen_framebuffer; break;
            case 0xcc: fn=(void*)gfx_create_offscreen_framebuffer; break;
            case 0xcd: fn=(void*)gfx_init_context; break;
            case 0xce: fn=(void*)gfx_set_context; break;
            case 0xd0: fn=(void*)gfx_put_pixel; break;
            case 0xd1: fn=(void*)gfx_draw_line; break;
            case 0xd2: fn=(void*)gfx_draw_rect; break;
            case 0xd3: fn=(void*)gfx_fill_rect; break;
            case 0xd4: fn=(void*)gfx_copy_frame_buffer; break;
            case 0xd5: fn=(void*)gfx_draw_image; break;
            case 0xd7: fn=(void*)gfx_copy_area; break;
            case 0xd8: fn=(void*)gfx_draw_arc; break;
            case 0xd9: fn=(void*)gfx_fill_arc; break;
            case 0xda: fn=(void*)gfx_draw_string; break;
            case 0xdc: fn=(void*)gfx_get_rgb_pixels; break;
            case 0xdd: fn=(void*)gfx_set_rgb_pixels; break;
            case 0xde: fn=(void*)gfx_flush; break;
            case 0xdf: fn=(void*)gfx_pixel_from_rgb; break;
            case 0xe0: fn=(void*)gfx_rgb_from_pixel; break;
            case 0xe1: fn=(void*)gfx_get_display_info; break;
            case 0xe2: fn=(void*)gfx_repaint; break;
            case 0xe3: fn=(void*)gfx_get_font; break;
            case 0xe4: fn=(void*)gfx_get_font_height; break;
            case 0xe5: fn=(void*)gfx_get_font_ascent; break;
            case 0xe6: fn=(void*)gfx_get_font_descent; break;
            case 0xe7: fn=(void*)gfx_get_string_width; break;
            case 0xe9: fn=(void*)gfx_create_image; break;
            case 0xeb: fn=(void*)gfx_destroy_image; break;
            case 0xee: fn=(void*)gfx_unk_ee; break;
            // WIPI input-method API. Zenonia 2 needs these in CB0.
            case 0x12c: fn=(void*)im_get_support_mode_count; break;
            case 0x12d: fn=(void*)im_get_supported_modes; break;
            case 0x12e: fn=(void*)im_set_current_mode; break;
            case 0x12f: fn=(void*)im_get_current_mode; break;
            case 0x130: fn=(void*)im_handle_input; break;
            case 0x338: fn=(void*)uic_get_time; break;
            // LGT/WIPI filesystem table (matches wie-lgt svc_ids.rs).
            case 0x190: fn=(void*)db_open; break;
            case 0x191: fn=(void*)db_read; break;
            case 0x192: fn=(void*)db_write; break;
            case 0x193: fn=(void*)db_close; break;
            case 0x194: fn=(void*)db_seek; break;
            case 0x195: fn=(void*)fs_file_attribute; break;
            case 0x196: fn=(void*)db_delete; break;
            case 0x197: fn=(void*)fs_rename; break;
            case 0x198: fn=(void*)fs_mkdir; break;
            case 0x199: fn=(void*)fs_rmdir; break;
            case 0x19a: fn=(void*)fs_list; break;
            case 0x19c: fn=(void*)db_available_storage; break;
            case 0x19f: fn=(void*)fs_tell; break;
            case 0x1a0: fn=(void*)db_exists; break;
            // WIPI media compatibility. Zenonia 2 ships SMAF/MMF resources;
            // these calls feed the real buffered clip + DS PCM/SMAF synth path.
            case 0x4b0: fn=(void*)media_clip_create; break;
            case 0x4b1: fn=(void*)media_clip_free; break;
            case 0x4b3: fn=(void*)media_clip_put_data; break;
            case 0x4b6: fn=(void*)media_noop4; break;
            case 0x4b8: fn=(void*)media_clip_get_volume; break;
            case 0x4b9: fn=(void*)media_clip_set_volume; break;
            case 0x4ba: fn=(void*)media_clip_play; break;
            case 0x4bd: fn=(void*)media_clip_stop; break;
            case 0x4c0: fn=(void*)media_noop4; break;
            case 0x4c1: fn=(void*)media_vibrator_on; break;
            case 0x4c2: fn=(void*)media_noop4; break;
            case 0x4c5: fn=(void*)media_clip_prepare; break;
            case 0x4c6: fn=(void*)media_noop4; break;
            case 0x4ce: fn=(void*)media_get_profile_volume; break;
            case 0x4d1: fn=(void*)media_set_device_enabled; break;
            case 0x4d2: fn=(void*)media_get_device_enabled; break;
            default: break;
        }
    } else if(table==0x1f8 && index==0x16) {
        fn=(void*)vendor_1f8_016;
    } else if(table==1) {
        switch(index) {
            case 0x3f6: fn=(void*)std_unk2; break;
            case 0x3f7: fn=(void*)std_sprintf; break;
            case 0x3fb: fn=(void*)std_atoi; break;
            case 0x403: fn=(void*)std_rand; break;
            case 0x404: fn=(void*)std_srand; break;
            case 0x405: fn=(void*)std_strcpy; break;
            case 0x406: fn=(void*)std_strncpy; break;
            case 0x407: fn=(void*)std_strcat; break;
            case 0x409: fn=(void*)std_strcmp; break;
            case 0x40a: fn=(void*)std_unk4; break;
            case 0x410: fn=(void*)std_strstr; break;
            case 0x411: fn=(void*)std_strlen; break;
            case 0x414: fn=(void*)std_memcpy; break;
            case 0x418: fn=(void*)std_memset; break;
            case 0x41a: fn=(void*)std_time; break;
            case 0x420: fn=(void*)std_localtime; break;
            case 0x424: fn=(void*)std_unk3; break;
            default: break;
        }
    }
    if(fn) {
        void *trace=make_known_trace_thunk(table,index,fn);
        return trace?trace:fn;
    }
    void *stub=make_unknown_thunk(table,index);
    return stub?stub:(void*)wipi_stub;
}

const WipiCletState *wipi_clet_state(void) { return &g_clet; }

bool wipi_start_clet(void) {
    printf("[CLET] enter start helper\n");
    if(!g_clet.registered) { printf("[CLET!] not registered\n"); return false; }
    if(!g_clet.callbacks[0]) { printf("[CLET!] cb0 NULL\n"); return false; }
    const uint32_t cb=g_clet.callbacks[0];
    printf("[CLET] cb0=%08lx thumb=%lu\n",(unsigned long)cb,(unsigned long)(cb&1u));
    if((cb&~1u)<0x02000000u || (cb&~1u)>=0x04000000u) {
        printf("[CLET!] cb0 outside RAM\n");
        return false;
    }
    typedef void (*Fn)(void);
    Fn f=(Fn)(uintptr_t)cb;
    printf("[CLET] flush D-cache\n");
    DC_FlushAll();
    printf("[CLET] invalidate I-cache\n");
    IC_InvalidateAll();
    printf("[SAV] Clet start backend=%s\n",ZenSav::BackendName());
    printf("[CLET>] calling cb0 now\n");
    // v005: call immediately; the previous VBlank wait was the actual hang.
    ZenBootProfile::reset();
    f();
    printf("[CLET<] cb0 returned\n");
    g_clet.started=true;
    g_audio_clet_started_ms=real_monotonic_ms();
    printf("[CLET] started=YES\n");
    return true;
}

void wipi_paint_clet(void) {
    paint_region(0,0,PHONE_W,PHONE_H);
}

static int runner_logical_wipi_for_bit(int bit) {
    if(bit==0) return -1;
    if(bit==1) return -2;
    if(bit==2) return -3;
    if(bit==3) return -4;
    int idx=bit-4;
    return (idx>=0 && idx<UI_TARGET_COUNT)?g_runner_targets[idx].wipi_key:0;
}

static void runner_release_all_logical(void) {
    for(int bit=0;bit<4+UI_TARGET_COUNT;bit++) if(g_ui_prev_logical&(1u<<bit)) {
        call_event(503,runner_logical_wipi_for_bit(bit));
    }
    g_ui_prev_logical=0;
}

void wipi_dispatch_keys(uint32_t down,uint32_t up) {
    (void)up;
    // SELECT is a DS-side display toggle, never a guest key. Exclude it even
    // when the diagnostic SINGLE/COMBO remapping UI is active.
    uint32_t rawHeld=keysHeld() & ~(KEY_TOUCH|KEY_LID|KEY_SELECT);
    uint32_t rawDown=down & ~(KEY_TOUCH|KEY_LID|KEY_SELECT);

    // v011: the runtime LOG overlay may stay visible while testing the game.
    // Only consume physical buttons when the user is actively capturing a
    // SINGLE/COMBO remap.  Merely having the LOG screen open must NOT block
    // game input (v010 did, which made every key appear dead).
    if(g_log_visible && (g_ui_capture_single>=0 || g_ui_capture_combo>=0)) {
        if(g_ui_capture_single>=0) {
            uint32_t key=runner_first_physical(rawDown);
            if(key) {
                int idx=g_ui_capture_single;
                g_ui_single_map[idx]=key;
                char msg[32]; snprintf(msg,sizeof(msg),"single %s=%s",g_runner_targets[idx].name,runner_physical_name(key));
                runner_ui_log_push_line(msg);
                g_ui_capture_single=-1;
                g_ui_dirty=true;
            }
        }
        if(g_ui_capture_combo>=0) {
            uint32_t a,b; runner_first_two_physical(rawHeld,&a,&b);
            if(a&&b) {
                int idx=g_ui_capture_combo;
                g_ui_combo_map1[idx]=a;
                g_ui_combo_map2[idx]=b;
                char msg[32]; snprintf(msg,sizeof(msg),"combo %s=%s+%s",g_runner_targets[idx].name,runner_physical_name(a),runner_physical_name(b));
                runner_ui_log_push_line(msg);
                g_ui_capture_combo=-1;
                g_ui_dirty=true;
            }
        }
        runner_ui_draw();
        return;
    }


    uint32_t logical=0;
    // D-pad remains fixed to WIPI directional codes.
    if(rawHeld&KEY_UP) logical|=1u<<0;
    if(rawHeld&KEY_DOWN) logical|=1u<<1;
    if(rawHeld&KEY_LEFT) logical|=1u<<2;
    if(rawHeld&KEY_RIGHT) logical|=1u<<3;

    uint32_t activeComboPhysical=0;
    for(int i=0;i<UI_TARGET_COUNT;i++) {
        if(g_ui_combo_map1[i]&&g_ui_combo_map2[i]) {
            uint32_t need=g_ui_combo_map1[i]|g_ui_combo_map2[i];
            if((rawHeld&need)==need) {
                logical|=1u<<(4+i);
                activeComboPhysical|=need;
            }
        }
    }
    for(int i=0;i<UI_TARGET_COUNT;i++) {
        uint32_t key=g_ui_single_map[i];
        if(key && (rawHeld&key) && !(activeComboPhysical&key)) logical|=1u<<(4+i);
    }

    if(!(keysHeld()&KEY_TOUCH) || g_log_visible || !g_rv_map_self) g_touch_logical=0;
    logical|=g_touch_logical;
    uint32_t logicalDown=logical & ~g_ui_prev_logical;
    uint32_t logicalUp=g_ui_prev_logical & ~logical;
    for(int bit=0;bit<4+UI_TARGET_COUNT;bit++) {
        const uint32_t mask=1u<<bit;
        const int wipi=runner_logical_wipi_for_bit(bit);
        if(logicalDown&mask) {
            static unsigned key_log_budget=12;
            if(key_log_budget) {
                printf("[KEY] %s k=%d\n",bit<4?(bit==0?"UP":bit==1?"DOWN":bit==2?"LEFT":"RIGHT"):g_runner_targets[bit-4].name,wipi);
                key_log_budget--;
            }
            g_trace_next_timer=false;
            g_trace_repaint_once=true;
            g_db_trace_budget=16;
            g_unknown_trace_budget=12;

            // Ragnarok's startup restart dialog is game state 0x16.  Its native
            // UI processor explicitly accepts EZ/FIRE (-5), but handleCletEvent
            // only forwards to that processor for selected handset input modes.
            // Route DS A directly only on this one dialog; all other states keep
            // the normal WIPI event path and README mapping.
            if(wipi==-5 && g_rv_game_state && g_rv_ui_key_handler && *g_rv_game_state==0x16u) {
                printf("[EZ] direct state=16 key=-5\n");
                g_rv_direct_ez_held=true;
                g_rv_ui_key_handler(-5);
            } else {
                call_event(502,wipi);
            }
        }
        if(logicalUp&mask) {
            if(wipi==-5 && g_rv_direct_ez_held) g_rv_direct_ez_held=false;
            else call_event(503,wipi);
        }
    }
    g_ui_prev_logical=logical;
}

