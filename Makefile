#---------------------------------------------------------------------------------
# Bouncy Bacon (PS3) - PSL1GHT build
#
#   make        -> bouncy-ps3.self
#   make pkg    -> bouncy-ps3.pkg (install via Package Manager on CFW, e.g. Evilnat/Cobra)
#   make test   -> headless desktop test build (no PS3 toolchain needed)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(MAKECMDGOALS)),test)
#---------------------------------------------------------------------------------
test:
	@mkdir -p build_test
	gcc -O2 -std=gnu99 -Wall -Iinclude source/game.c source/render.c source/audio.c \
	    source/main.c source/assets_data.c test/plat_test.c -lm -o build_test/bouncy_test
	@echo "run: ./build_test/bouncy_test <outdir> <frames> \"<script>\" \"<shots>\""
.PHONY: test
#---------------------------------------------------------------------------------
else

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

TARGET		:=	bouncy-ps3
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
PKGFILES	:=	$(CURDIR)/pkgfiles

TITLE		:=	Bouncy Bacon
APPID		:=	BBCN00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000
ICON0		:=	$(CURDIR)/pkgfiles/ICON0.PNG
SFOXML		:=	$(CURDIR)/sfo.xml

CFLAGS		=	-O3 -ffast-math -fno-strict-aliasing -Wall -std=gnu99 -mcpu=cell -mtune=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS	:=	-lgcm_sys -lrsx -lsysutil -lio -laudio -lm -lrt -llv2

LIBDIRS	:= $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

# plat_test.c lives in test/ so only the PS3 platform file is in source/
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT)*.pkg build_test

pkg: $(BUILD) $(OUTPUT).pkg

run:
	ps3load $(OUTPUT).self

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

# the generated asset tables are large; don't spend optimiser time on them
assets_data.o: CFLAGS := $(subst -O3,-O0,$(CFLAGS))

-include $(DEPENDS)

endif
endif
