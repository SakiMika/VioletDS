#!/usr/bin/env python3
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[1]
shim=(ROOT/'source/wipi_shim.cpp').read_text(errors='replace')
main=(ROOT/'source/main.cpp').read_text(errors='replace')
mk=(ROOT/'Makefile').read_text(errors='replace')
layout=(ROOT/'source/ragnarok_layout_policy.h').read_text(errors='replace')

assert 'Width = 240' in layout and 'Height = 320' in layout
assert 'GUEST_TIME_NUM=1' in shim and 'GUEST_TIME_DEN=1' in shim
# Preserve the exact mapping defined by the original project README:
# A=EZ/FIRE, B=CLR, X=0, Y=*, R=#, L+A/B/X/Y=1/3/7/9, START=Call,
# SELECT=DS-side bottom display toggle. D-pad is fixed directions.
expected_targets=[
    ('EZ', '-5'), ('CLR', '-16'),
    ('1', "'1'"), ('3', "'3'"), ('5', "'5'"),
    ('7', "'7'"), ('9', "'9'"), ('*', "'*'"),
    ('0', "'0'"), ('#', "'#'"), ('Call', '-10'),
]
import re
m=re.search(r'g_runner_targets\[UI_TARGET_COUNT\]\s*=\s*\{(.*?)\n\};',shim,re.S)
assert m, 'target table missing'
actual_targets=re.findall(r'\{\s*"([^"\n]+)"\s*,\s*([^}\n]+?)\s*\}',m.group(1))
assert [(name,key.strip()) for name,key in actual_targets]==expected_targets,actual_targets
m=re.search(r'static void runner_ui_defaults\(void\)\s*\{(.*?)\n\}',shim,re.S)
assert m, 'mapping initializer missing'
init=m.group(1)
expected_singles={0:'KEY_A',1:'KEY_B',7:'KEY_Y',8:'KEY_X',9:'KEY_R',10:'KEY_START'}
actual_singles={int(i):key for i,key in re.findall(r'g_ui_single_map\[(\d+)\]\s*=\s*(KEY_\w+)',init)}
assert actual_singles==expected_singles,actual_singles
assert 'const int targets[4]={2,3,5,6}' in init
assert 'const uint32_t keys[4]={KEY_A,KEY_B,KEY_X,KEY_Y}' in init
assert 'g_ui_combo_map1[targets[i]]=KEY_L' in init
assert 'g_ui_combo_map2[targets[i]]=keys[i]' in init
assert 'g_bottom_half_visible=true' in init
# Combo keys must suppress the corresponding singles, so L+A sends only 1.
assert 'activeComboPhysical|=need' in shim
assert '!(activeComboPhysical&key)' in shim
# SELECT is not a guest WIPI key; it switches SUB pixels on/off immediately.
assert 'keysHeld() & ~(KEY_TOUCH|KEY_LID|KEY_SELECT)' in shim
assert 'rawDown=down & ~(KEY_TOUCH|KEY_LID|KEY_SELECT)' in shim
assert 'if((down&KEY_SELECT)' not in shim
assert 'g_bottom_half_visible=!g_bottom_half_visible' not in shim

assert 'wipi_toggle_minimap();' not in shim.split('void wipi_dispatch_keys(',1)[1]
assert 'START=Call, SELECT=unused' in main

# Ragnarok startup restart prompt: native UI state 0x16 explicitly accepts
# EZ/FIRE (-5).  The relocation-safe bridge bypasses only this one handset
# dialog when handleCletEvent's internal input mode does not forward it.
assert 'lgt_translate_ptr(&image,0x015009C0u)' in main
assert 'lgt_translate_ptr(&image,0x0000B110u)' in main
assert 'wipi_set_ragnarok_ez_bridge(rv_game_state,rv_ui_key_handler)' in main
assert 'if(wipi==-5 && g_rv_game_state && g_rv_ui_key_handler && *g_rv_game_state==0x16u)' in shim
assert 'g_rv_ui_key_handler(-5);' in shim
assert 'g_rv_direct_ez_held=true' in shim
assert 'if(wipi==-5 && g_rv_direct_ez_held) g_rv_direct_ez_held=false' in shim

# Runtime logging must not blank/redraw the SUB console for every printf.
runner_printf_body=shim.split('static int runner_printf',1)[1].split('#define printf',1)[0]
assert 'runner_ui_draw();' not in runner_printf_body
runner_draw_body=shim.split('static void runner_ui_draw(void) {',1)[1].split('struct DummyMediaClip',1)[0]
assert '\x1b[2J' not in runner_draw_body
poll_body=shim.split('void wipi_poll(void)',1)[1].split('uint32_t wipi_get_import_table',1)[0]
assert 'now-g_log_last_draw_ms>=100u' in poll_body
assert 'runner_ui_draw();' in poll_body
# Width fits TOP while vertical pixels stay native; SUB keeps settings/log.
present=shim.split('void wipi_present(void)',1)[1].split('// Boot probe',1)[0]
assert 'src[x*PHONE_W/DS_W]' in present
assert 'g_phone_fb+y*PHONE_W' in present
assert 'y*PHONE_H/DS_H' not in present
assert 'HALF_H' not in present
assert 'copy_phone_rect_to_sub_buf' not in present
assert 'gfx_flush_source_to_phone(source)' in shim
assert 'extern "C" int gfx_flush(int kind, uint32_t source' in shim
assert 'dmaCopy(g_sub_comp,g_ds_fb,LCD_BYTES)' in shim
assert 'dmaCopy(g_sub_comp,g_sub_fb,LCD_BYTES)' in shim
present=shim.split('void wipi_present(void)',1)[1].split('// Boot probe',1)[0]
assert 'ZenRenderFast::fill16(g_ds_fb' not in present
assert 'rv_png_decode_cached(out_image,src,len)' in shim
assert 'nitro:/_port/rv_png_indices.bin' in shim
assert '#include "rv_png_cache_table.h"' in shim
assert 'case 0xe9: fn=(void*)gfx_create_image' in shim
assert 'case 0xeb: fn=(void*)gfx_destroy_image' in shim
assert 'struct LgtGraphicsView' in shim and 'RvFramebufferAllocation' in shim
assert 'extern "C" void *fb_ptr(uint32_t fbp)' in shim
assert 'zen_native_layout_install' not in main
assert 'zen_native_minimap_install' not in main
assert 'zen_native_boot_install' not in main
assert 'TARGET      := Ragnarok_Violet' in mk
assert 'GAME_SUBTITLE2  := WIPI NDS' in mk
assert 'filter-out zen_native_layout.cpp zen_native_minimap.cpp zen_native_boot.cpp zen_predecoded_audio.cpp' in mk
# Build regression from 2026-10-20: do not reference the excluded Zenonia
# title108 PCM cache implementation from the live Ragnarok audio dispatcher.
assert 'ZenDecodedPcm' not in shim
assert 'zen_load_title_pcm' not in shim
# Ragnarok uses the standard four-word LGT/WIPI timer ABI.  r2 is timeout_high
# and r3 is the callback parameter; do not restore the Zenonia owner-pointer hack.
assert 'uint32_t timeout_high, void *param' in shim
assert '((uint64_t)timeout_high<<32)' in shim
assert 'g_timers[i].param=param' in shim
assert 'used_lgt_owner' not in shim
assert '[T>] s=%d cb=%08lx' in shim
assert '[TPC] old=%08lx p=%08lx' in shim
assert '[TC0] %04x %04x %04x %04x' in shim
assert '[K] %03lx:%03lx' in shim
assert 'make_known_trace_thunk(table,index,fn)' in shim
assert '[T<] s=%d ret rp=%lu fl=%lu im=%lu' in shim
assert 'g_trace_next_timer=true;' in shim
# Diagnostic regressions: make boot state visible even if native guest never
# returns. LOG gear and touch must be connected to the actual SUB renderer.
assert 'wipi_boot_stage("01 VIDEO READY")' in main
assert 'wipi_boot_stage("05 CALL ENTRY")' in main
assert 'wipi_boot_stage("06 CALL INIT")' in main
assert 'wipi_boot_stage("08 START CLET")' in main
assert 'wipi_boot_stage("09 PAINT CLET")' in main
assert 'wipi_boot_stage("10 GAME LOOP")' in main
assert main.index('wipi_boot_stage("01 VIDEO READY")') < main.index('wipi_set_log_visible(true);', main.index('int main(void)'))
# Normal boot closes the diagnostic console once GAME LOOP is alive; LOG can be
# reopened by touch. Fatal paths still open it explicitly.
assert 'wipi_set_log_visible(false);' in main.split('int main(void)',1)[1]
assert main.index('wipi_debug_dump("after paint")') < main.index('wipi_set_log_visible(false);')
assert 'draw_log_button_to(sub);' in shim.split('void wipi_present(void)',1)[1].split('void wipi_set_log_visible',1)[0]
assert 'touchRead(&touch);' in main and 'wipi_handle_touch(down, touch.px, touch.py);' in main
assert 'runner_ui_log_push_line(stage);' in shim
assert 'memcmp(c->data,"MMMD",4)==0) return media_decode_smaf(c)' in shim
assert 'memcmp(c->data,"RIFF",4)==0) return media_decode_wav(c)' in shim

nitro=ROOT/'nitrofs'
all_files=[p for p in nitro.rglob('*') if p.is_file()]
game_files=[p for p in all_files if '_port' not in p.relative_to(nitro).parts]
png_files=[p for p in game_files if p.suffix.lower()=='.png']
assert len(game_files)==1279, len(game_files)
assert len(all_files)==1280, len(all_files)  # + one private PNG index cache
assert len(png_files)==434, len(png_files)
assert (nitro/'binary.mod').stat().st_size==474040
assert (nitro/'res/font').stat().st_size==2711
assert (nitro/'_port/rv_png_indices.bin').stat().st_size==1724201

# The critical regression: PNG bytes must remain byte-for-byte PNG resources.
# Ragnarok's native font loader searches for PLTE and rewrites palette bytes
# before calling CreateImage. Replacing embedded PNGs with RVP1 makes that code
# derive negative chunk offsets and hang inside the timer callback.
png_magic=b'\x89PNG\r\n\x1a\n'
def png_len(blob,start):
    assert blob[start:start+8]==png_magic
    pos=start+8
    while True:
        n=struct.unpack_from('>I',blob,pos)[0]
        typ=blob[pos+4:pos+8]
        pos += 12+n
        assert pos<=len(blob)
        if typ==b'IEND': return pos-start

png_count=0
embedded_png=0
for p in game_files:
    b=p.read_bytes(); at=0
    while True:
        at=b.find(png_magic,at)
        if at<0: break
        n=png_len(b,at)
        png_count+=1
        if p.suffix.lower()!='.png':
            embedded_png+=1
            assert at>=2 and struct.unpack_from('<H',b,at-2)[0]==n, (p,at,n)
        at += n
assert png_count==1390, png_count
assert embedded_png==956, embedded_png
font=(nitro/'res/font').read_bytes()
assert font.count(png_magic)==8 and font.count(b'PLTE')==8
assert b'RVP1' not in font

cache_header=(ROOT/'source/rv_png_cache_table.h').read_text(errors='replace')
assert 'kRvPngCacheEntryCount = 1122u' in cache_header
assert 'struct RvPngCacheEntry' in cache_header
packer=(ROOT/'tools/pack_ragnarok_assets.py').read_text(errors='replace')
assert 'Preserve game resources byte-for-byte' in packer
assert 'rv_png_indices.bin' in packer
assert 'convert_embedded_pngs' not in packer
assert 'sig=%02x%02x%02x%02x' in shim
assert '[PNG] create %lux%lu pal=%lu alpha=%lu' in shim
# Timer callback tracing must surround the guest callback so unresolved imports
# inside it are visible and a missing return is distinguishable from a healthy tick.
enter=shim.index("g_trace_next_timer=true;")
call=shim.index("cb(timer,param);", enter)
leave=shim.index("g_trace_next_timer=false;", call)
assert enter < call < leave

# Startup handset-reboot bypass is surgical and opcode-validated.
main_cpp=(ROOT/'source'/'main.cpp').read_text(errors='ignore')
assert '0x000027D8u' in main_cpp
assert 'old==0xD003u' in main_cpp
assert '*restart_gate=0x46C0u' in main_cpp
assert '0x0000B110u' in main_cpp  # diagnostic EZ fallback retained

# Ragnarok save persistence regression.  The public build is now a single
# NDS-mode compatibility code CDLG selects 128 KiB EEPROM in stock melonDS; internal port ID remains RVDS.
sav=(ROOT/'source/zen_sav_backend.cpp').read_text(errors='replace')
savh=(ROOT/'source/zen_sav_backend.h').read_text(errors='replace')
header_tool=(ROOT/'tools/patch_nds_header.py').read_text(errors='replace')
workflow=(ROOT/'.github/workflows/build-nds.yml').read_text(errors='replace')
assert 'RAGNAROK.sav' in sav  # fallback code may remain, but no second artifact is built
assert 'RAGNAROK_SAVE_SLOTS = 3u' in sav
assert 'PERSISTENT_NAMES    = 5u' in sav
assert 'FAT_RECORD_CAPACITY = 16u' in sav  # physical COW records, not game slots
assert 'is_save_number_name' in sav
assert 'n<RAGNAROK_SAVE_SLOTS' in sav
assert 'ascii_ieq(base,"option")' in sav
assert 'ascii_ieq(base,"list")' in sav
assert 'NATIVE_TOTAL_SIZE   = 128u*1024u' in sav
assert 'NATIVE_ENTRY_COUNT  = PERSISTENT_NAMES' in sav
assert 'if(ascii_ieq(base,"option") || ascii_ieq(base,"option.sav")) return 3;' in sav
assert 'if(ascii_ieq(base,"list")) return 4;' in sav
assert 'Losing list makes every slot appear New Game' in sav
assert 'drop legacy save3..save15 entries' in sav
assert '-DRV_CART_SAVE_PROFILE=0' in mk
assert 'SAVE_PROFILE ?=' not in mk and '\nSAVE_PROFILE=' not in mk
assert r'ROM_GAMECODE := \#\#\#\#' in mk
assert 'TARGET      := Ragnarok_Violet' in mk
assert "GAMECODE = b'####'" in header_tool
assert "choices=('melonds', 'dldi')" not in header_tool
assert 'matrix:' not in workflow and 'SAVE_PROFILE=' not in workflow
assert "Ragnarok Violet.nds" in workflow
assert "Unexpected duplicate ROM: Ragnarok_Violet.nds" in workflow
assert not (ROOT/'build_dldi.bat').exists()
assert not (ROOT/'BUILD_DSI_WINDOWS.bat').exists()
assert '*.sav' in (ROOT/'.gitignore').read_text(errors='replace')

# Save-slot timestamp ABI. Ragnarok imports UIC GetTime (1fb:338), which must
# write six 32-bit fields; an unresolved success stub leaves stack garbage.
assert 'case 0x338: fn=(void*)uic_get_time' in shim
assert 'extern "C" void uic_get_time(uint32_t component, int32_t *out)' in shim
for expr in ['out[0]=t->tm_sec', 'out[1]=t->tm_min', 'out[2]=t->tm_hour',
             'out[3]=t->tm_mday', 'out[4]=t->tm_mon', 'out[5]=t->tm_year']:
    assert expr in shim
assert 'sizeof(GuestTm32)==0x2c' in shim
# binary.mod really imports WIPI table 0x1fb index 0x338.
assert (0x1fb).to_bytes(4,'little') + (0x338).to_bytes(4,'little') in (nitro/'binary.mod').read_bytes()

# LGT 0x190..0x1a0 must be filesystem ABI, not the old Zenonia DB-record ABI.
assert 'uint32_t cursor;' in shim and 'uint32_t read_cursor;' not in shim and 'uint32_t write_cursor;' not in shim
assert 'case 0x195: fn=(void*)fs_file_attribute' in shim
assert 'case 0x197: fn=(void*)fs_rename' in shim
assert 'case 0x198: fn=(void*)fs_mkdir' in shim
assert 'case 0x199: fn=(void*)fs_rmdir' in shim
assert 'case 0x19a: fn=(void*)fs_list' in shim
assert 'case 0x19f: fn=(void*)fs_tell' in shim
assert 'out[0]=0; out[1]=0; out[2]=len;' in shim
assert 'Do NOT clamp at EOF' in shim
assert '[FS] reopen' not in shim
assert 'every MC_fsOpen creates a fresh handle' in shim
assert 'A WIPI file descriptor is dead after close' in shim
assert 'ragnarok_encoded_file_valid' in shim
assert '[SLOT] %s len=%lu guest-check=%s' in shim

# The supplied Ragnarok binary confirms the writable DB naming convention.
binmod=(nitro/'binary.mod').read_bytes()
assert b'save%d\x00' in binmod
assert b'option\x00' in binmod

# Single-ROM output policy
assert 'mv -f Ragnarok_Violet.nds "Ragnarok Violet.nds"' in mk
assert 'copy /y "Ragnarok_Violet.nds" "Ragnarok Violet.nds"' not in (ROOT / 'build.bat').read_text(encoding='utf-8', errors='ignore')
assert 'out.write_bytes(data)' not in workflow

print(f'OK: {len(game_files)} pristine game files; {png_count} PNG instances ({embedded_png} embedded); 1122 cached IDAT streams; mapping/display/time/save checks passed; single NDS output enforced')

# No stale text-note files are shipped in the cleaned source package.
assert not list(ROOT.rglob('*.txt')), 'stale .txt files remain in package'

