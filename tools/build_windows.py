#!/usr/bin/env python3
"""Single DS-mode build using native devkitARM executables, no MSYS make."""
from pathlib import Path
import os, subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
os.chdir(ROOT)
def run(args):
    subprocess.run([str(x) for x in args],check=True)
pro=Path(os.environ.get('DEVKITPRO') or ('D:/devkitPro' if Path('D:/devkitPro').exists() else 'C:/devkitPro'))
arm=Path(os.environ.get('DEVKITARM') or pro/'devkitARM')
if not (arm/'bin/arm-none-eabi-g++.exe').is_file():
    pro=next((p for p in [Path('D:/devkitPro'),Path('C:/devkitPro')] if (p/'devkitARM/bin/arm-none-eabi-g++.exe').is_file()),pro)
    arm=pro/'devkitARM'
compiler=arm/'bin/arm-none-eabi-g++.exe'
run([sys.executable,'tests/ragnarok_firstpass_test.py'])
build=ROOT/'build';build.mkdir(exist_ok=True)
flags=['-D__NDS__','-DARM9','-DRV_CART_SAVE_PROFILE=1','-DZEN_CONSERVATIVE_CALLBACK_CACHE=0',
       '-g','-Wall','-Wextra','-O2','-ffunction-sections','-fdata-sections',
       '-march=armv5te','-mtune=arm946e-s','-mthumb-interwork',
       '-fno-rtti','-fno-exceptions','-fno-strict-aliasing','-std=gnu++17','-iquote','source','-Ibuild']
for inc in ['calico/include','libnds/include','portlibs/nds/include','portlibs/armv5te/include','portlibs/armv4t/include']:
    flags.append('-I'+(pro/inc).as_posix())
objects=[]
for name in ['embedded_assets','lgt_loader','main','wipi_shim','zen_sav_backend']:
    obj=build/(name+'.o');objects.append(obj)
    print('[COMPILE]',name,flush=True)
    run([compiler,*flags,'-MMD','-MP','-MF',build/(name+'.d'),'-c',ROOT/'source'/(name+'.cpp'),'-o',obj])
link=['-L'+(pro/x).as_posix() for x in ['calico/lib','libnds/lib','portlibs/nds/lib','portlibs/armv5te/lib','portlibs/armv4t/lib']]
run([compiler,*link,'-specs='+(pro/'calico/share/ds9.specs').as_posix(),'-g',
     '-march=armv5te','-mtune=arm946e-s','-mthumb-interwork','-Wl,-Map,build/Ragnarok_Violet.map',
     *objects,'-lfilesystem','-lfat','-lnds9','-lcalico_ds9','-o','Ragnarok_Violet.elf'])
run([pro/'tools/bin/ndstool.exe','-c','Ragnarok Violet.nds','-9','Ragnarok_Violet.elf',
     '-7',pro/'calico/bin/ds7_maine.elf','-b','icon.bmp','Ragnarok Violet','-h','0x8000',
     '-g','CDLG','01','Ragnarok','01','-d','nitrofs'])
run([sys.executable,'tools/patch_nds_header.py','Ragnarok Violet.nds'])
# Obsolete generated ROMs only; keep all save files intact.
for name in ['Ragnarok Violet DS Test.nds','Ragnarok_Violet.nds']:
    old=ROOT/name
    if old.is_file():old.unlink()
print('[SUCCESS] Ragnarok Violet.nds (128 KiB EEPROM; also runs on DSi)')
