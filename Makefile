.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITPRO=/opt/devkitpro; export DEVKITARM=$(DEVKITPRO)/devkitARM")
endif

include $(DEVKITARM)/ds_rules

GAME_TITLE      := Ragnarok Violet
GAME_SUBTITLE1  := Gravity
GAME_SUBTITLE2  := WIPI NDS

# Single DS-mode build. DSi can run normal .nds software, so there is no
# separate DSi artifact/profile.
TARGET      := Ragnarok_Violet
ROM_GAMECODE := CDLG
BUILD       := build
SOURCES     := source
INCLUDES    := source
DATA        :=
GRAPHICS    :=
AUDIO       :=
ICON        := icon.bmp
NITRO       := nitrofs
PYTHON      ?= python

ARCH        := -march=armv5te -mtune=arm946e-s -mthumb-interwork
CALLBACK_CACHE ?= 0
SAVE_DEFS   := -DRV_CART_SAVE_PROFILE=1
CFLAGS      := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections $(ARCH) $(INCLUDE) -DARM9 -DZEN_CONSERVATIVE_CALLBACK_CACHE=$(CALLBACK_CACHE) $(SAVE_DEFS)
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -fno-strict-aliasing -std=gnu++17
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS        := -lnds9

ifneq ($(strip $(NITRO)),)
LIBS        := -lfilesystem -lfat $(LIBS)
endif

LIBDIRS     := $(LIBNDS) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR) $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

ifneq ($(strip $(ICON)),)
export GAME_ICON := $(CURDIR)/$(ICON)
endif
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
CPPFILES := $(filter-out zen_native_layout.cpp zen_native_minimap.cpp zen_native_boot.cpp zen_predecoded_audio.cpp,$(CPPFILES))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifneq ($(strip $(NITRO)),)
export NITRO_FILES := $(CURDIR)/$(NITRO)
endif

export LD := $(CXX)
export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)
export INCLUDE := $(foreach dir,$(INCLUDES),-iquote $(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all
all: $(BUILD)

$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@rm -f "Ragnarok Violet.nds"
	@mv -f Ragnarok_Violet.nds "Ragnarok Violet.nds"

clean:
	@echo clean ...
	@rm -fr build Ragnarok_Violet.elf Ragnarok_Violet.nds Ragnarok_Violet.map "Ragnarok Violet.nds"

else
$(OUTPUT).nds: $(OUTPUT).elf $(NITRO_FILES) $(GAME_ICON)
	@echo "creating Ragnarok Violet NDS: ARM9@0x8000 gamecode=$(ROM_GAMECODE)..."
	@ndstool -c $@ -9 $< $(_ARM7_ELF) -b $(GAME_ICON) "$(GAME_TITLE)" -h 0x8000 -g "$(ROM_GAMECODE)" 01 "Ragnarok" 01 $(_ADDFILES)
	@$(PYTHON) "$(dir $(OUTPUT))tools/patch_nds_header.py" "$@"

$(OUTPUT).elf: $(OFILES)
$(OFILES_SOURCES):

-include $(DEPSDIR)/*.d
endif
