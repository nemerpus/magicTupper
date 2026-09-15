.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error Define DEVKITPRO: instala devkitPro con switch-dev)
endif
include $(DEVKITPRO)/libnx/switch_rules
TARGET := magictupper
BUILD := build
APP_TITLE := MagicTupper
APP_AUTHOR := MagicTupper
APP_VERSION := 0.4.0-rc.3
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
export PKG_CONFIG_PATH := $(PORTLIBS)/lib/pkgconfig
CFLAGS := -g -Wall -O2 -ffunction-sections -finput-charset=UTF-8 -fexec-charset=UTF-8 $(ARCH) -D__SWITCH__ $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions $(shell $(PORTLIBS)/bin/aarch64-none-elf-pkg-config --cflags sdl2 SDL2_ttf libcurl)
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -ljson-c $(shell $(PORTLIBS)/bin/aarch64-none-elf-pkg-config --libs --static SDL2_ttf sdl2 libcurl) -lnx
USBHSFS ?= 1
USB_DEBUG ?= 0
ifeq ($(USBHSFS),1)
CXXFLAGS += -DMAGICTUPPER_USBHSFS
ifeq ($(USB_DEBUG),1)
CXXFLAGS += -DMAGICTUPPER_USB_DIAGNOSTICS
LIBS := -lusbhsfsd $(LIBS)
else
LIBS := -lusbhsfs $(LIBS)
endif
endif
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CXX)
export OFILES := main.o install.o
export INCLUDE := -I$(LIBNX)/include -I$(PORTLIBS)/include -I$(CURDIR)/build
export LIBPATHS := -L$(LIBNX)/lib -L$(PORTLIBS)/lib
export NROFLAGS := --nacp=$(CURDIR)/$(TARGET).nacp --icon=$(CURDIR)/icon.jpg
.PHONY: all $(BUILD)
all: $(BUILD)
$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
else
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(TOPDIR)/icon.jpg
$(OUTPUT).elf: $(OFILES) $(TOPDIR)/Makefile
-include $(OFILES:.o=.d)
endif
