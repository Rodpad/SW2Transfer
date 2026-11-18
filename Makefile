#---------------------------------------------------------------------------------
# SW2Transfer for hbmenu (Switch 2 homebrew, kaiHEN): devkitPro + ounce-devkit.
#   export DEVKITPRO=/opt/devkitpro && make
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libounce/switch2_rules

APP_TITLE	:=	SW2Transfer
APP_AUTHOR	:=	Rodpad
APP_VERSION	:=	1.4.2

TARGET		:=	sw2transfer
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	source
ROMFS		:=	romfs
ICON		:=	sw2transfer.jpg

ARCH	:=	$(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS	:=	-g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -DNDEBUG -DAPP_VERSION=\"$(APP_VERSION)\" -I$(PORTLIBS)/include/freetype2
CXXFLAGS	:= $(CFLAGS) -std=gnu++17
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS	:= -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lfreetype -lharfbuzz -lpng16 -lbz2 -lz -lnx
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export LD	:=	$(CXX)
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES 	:=	$(OFILES_SRC)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_ICON := $(TOPDIR)/$(ICON)
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp --romfsdir=$(CURDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@cmp -s tools/sw2xfer.py $(ROMFS)/web/sw2xfer.py || (cp tools/sw2xfer.py $(ROMFS)/web/sw2xfer.py && rm -f $(TARGET).nro)
	@[ ! -f $(TARGET).nro ] || [ -z "$$(find $(ROMFS) -type f -newer $(TARGET).nro)" ] || rm -f $(TARGET).nro
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS	:=	$(OFILES:.o=.d)
all	:	$(OUTPUT).nro
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)
-include $(DEPENDS)
endif
