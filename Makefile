# Version number notes!
# Before building release version, bump the version number thus:
# 
# 1. Tag the master with: git tag -a v1.1 then enter an annotation message (it's like a commit message)
# 2. The above snippet used v1.1 as example..... change that v number to whatever you need!
# 3. Then try: git describe to see what version is displayed
# 4. Then run make in usual way


#You can build this example in three ways:
# 'separate' - Separate espfs and binaries, no OTA upgrade
# 'combined' - Combined firmware blob, no OTA upgrade
# 'ota' - Combined firmware blob with OTA upgrades.
#Please do a 'make clean' after changing this.
#OUTPUT_TYPE=separate
#OUTPUT_TYPE=combined
OUTPUT_TYPE=ota

# tag each release with "git tag -a vM.m" where M is the major version number
# and m is the minor version number

# version is MAJOR.minor-commit (date hash)
# if there has not been a commit since the last tag, commit and hash are left out
# MAJOR is the major version number (change when protocol changes)
# minor is the minor version number
# commit is the number of commits since the last tag
# date is the date of the build
# hash is a unique substring of the hash of the last commit

# Ensure --long tag added: GITDESC=$(shell git describe --long master)

GITDESC=$(shell git describe --long master)
GITDESC_WORDS=$(subst -, ,$(GITDESC))
GIT_VERSION=$(word 1,$(GITDESC_WORDS))
GIT_COMMIT=$(word 2,$(GITDESC_WORDS))
GIT_HASH=$(word 3,$(GITDESC_WORDS))
ifeq ($(GIT_COMMIT),)
  GIT_COMMIT_HASH_SUFFIX=
else
  GIT_COMMIT_HASH_SUFFIX=$(subst -, ,-$(GIT_COMMIT))-$(GIT_HASH)
endif
VERSION=$(GIT_VERSION) ($(shell date "+%Y-%m-%d %H:%M:%S")$(GIT_COMMIT_HASH_SUFFIX))
#VERSION=1.1 ($(shell date "+%Y-%m-%d %H:%M:%S")$(GIT_COMMIT_HASH_SUFFIX))
$(info VERSION $(VERSION))

#SPI flash size, in K
ESP_SPI_FLASH_SIZE_K=2048
#Amount of the flash to use for the image(s)
ESP_SPI_IMAGE_SIZE_K=1024
#0: QIO, 1: QOUT, 2: DIO, 3: DOUT
ESP_FLASH_MODE=0
#0: 40MHz, 1: 26MHz, 2: 20MHz, 0xf: 80MHz
#ESP_FLASH_FREQ_DIV=0xf
ESP_FLASH_FREQ_DIV=15

ifeq ("$(OUTPUT_TYPE)","separate")
#In case of separate ESPFS and binaries, set the pos and length of the ESPFS here. 
ESPFS_POS = 0x18000
ESPFS_SIZE = 0x28000
endif

# Output directors to store intermediate compiled files
# relative to the project directory
BUILD_BASE	= build
FW_BASE     = firmware

# Base directory for the compiler. Needs a / at the end; if not set it'll use the tools that are in
# the PATH.
XTENSA_TOOLS_ROOT ?= 

# base directory of the ESP8266 SDK package, absolute
# REMINDER: If changing from NONOS SDK, might need to adjust ESPMISSINGINCLUDES.H ; Watch for compile errors after changing SDK.
#SDK_BASE	?= /opt/Espressif/ESP8266_SDK
#SDK_BASE	?= $(abspath ../esp_iot_sdk_v1.5.2)
#SDK_BASE	?= $(abspath /media/data/Git/ESP8266_NONOS_SDK)
SDK_BASE	?= $(abspath /media/sf_Documents/GitHubVonszarvas/Parallax-ESP/ESP8266_NONOS_SDK)
#SDK_BASE	?= $(abspath ./esp_iot_sdk_v2.0.0.p1)
#SDK_BASE ?= $(abspath ./new/ESP8266_NONOS_SDK)

# Opensdk patches stdint.h when compiled with an internal SDK. If you run into compile problems pertaining to
# redefinition of int types, try setting this to 'yes'.
#USE_OPENSDK?=no
USE_OPENSDK?=yes

USE_AT?=no

#Esptool.py path and port
ESPTOOL		?= esptool.py
ESPPORT		?= /dev/ttyUSB0
#ESPDELAY indicates seconds to wait between flashing the two binary images
ESPDELAY	?= 3
ESPBAUD		?= 460800

#Appgen path and name
APPGEN		?= $(SDK_BASE)/tools/gen_appbin.py

# name for the target project
TARGET		= httpd

# which modules (subdirectories) of the project to include in compiling
MODULES		= user parallax esp-link-stuff
EXTRA_INCDIR	= include libesphttpd/include

# libraries used in this project, mainly provided by the SDK
LIBS		= c gcc hal phy pp net80211 wpa main lwip crypto
#Add in esphttpd lib
LIBS += esphttpd

ifeq ("$(USE_AT)","yes")
LIBS += at airkiss wps smartconfig ssl
endif

# compiler flags using during compilation of source files
CFLAGS		= -Os -ggdb -std=gnu99 -Werror -Wpointer-arith -Wundef -Wall -Wl,-EL -fno-inline-functions \
		-nostdlib -mlongcalls -mtext-section-literals  -D__ets__ -DICACHE_FLASH \
		-Wno-address -DVERSION=\""$(VERSION)"\" $(EXTRA_CFLAGS)

ifeq ("$(USE_AT)","yes")
CFLAGS += -DUSE_AT
endif

# linker flags used to generate the main object file
LDFLAGS		= -nostdlib -Wl,--no-check-sections -u call_user_start -Wl,-static


# various paths from the SDK used in this project
SDK_LIBDIR	= lib
SDK_LDDIR	= ld
SDK_INCDIR	= include include/json

# select which tools to use as compiler, librarian and linker
CC		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-gcc
AR		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-ar
LD		:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-gcc
OBJCOPY	:= $(XTENSA_TOOLS_ROOT)xtensa-lx106-elf-objcopy

#Additional (maybe generated) ld scripts to link in
EXTRA_LD_SCRIPTS:=


####
#### no user configurable options below here
####
SRC_DIR		:= $(MODULES)
BUILD_DIR	:= $(addprefix $(BUILD_BASE)/,$(MODULES))

SDK_LIBDIR	:= $(addprefix $(SDK_BASE)/,$(SDK_LIBDIR))
SDK_INCDIR	:= $(addprefix -I$(SDK_BASE)/,$(SDK_INCDIR))

SRC		:= $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))
ASMSRC		= $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.S))
OBJ		= $(patsubst %.c,$(BUILD_BASE)/%.o,$(SRC))
OBJ		+= $(patsubst %.S,$(BUILD_BASE)/%.o,$(ASMSRC))
APP_AR		:= $(addprefix $(BUILD_BASE)/,$(TARGET)_app.a)


V ?= $(VERBOSE)
ifeq ("$(V)","1")
Q :=
vecho := @true
else
Q := @
vecho := @echo
endif

ifeq ("$(USE_OPENSDK)","yes")
CFLAGS		+= -DUSE_OPENSDK
else
CFLAGS		+= -D_STDINT_H
endif

ifeq ("$(GZIP_COMPRESSION)","yes")
CFLAGS		+= -DGZIP_COMPRESSION
endif

ifeq ("$(USE_HEATSHRINK)","yes")
CFLAGS		+= -DESPFS_HEATSHRINK
endif

ifeq ("$(ESPFS_POS)","")
#No hardcoded espfs position: link it in with the binaries.
LIBS += webpages-espfs
else
#Hardcoded espfs location: Pass espfs position to rest of code
CFLAGS += -DESPFS_POS=$(ESPFS_POS) -DESPFS_SIZE=$(ESPFS_SIZE)
endif

ifeq ("$(OUTPUT_TYPE)","ota")
CFLAGS += -DOTA_FLASH_SIZE_K=$(ESP_SPI_IMAGE_SIZE_K)
endif


#Define default target. If not defined here the one in the included Makefile is used as the default one.
default-tgt: all

define maplookup
$(patsubst $(strip $(1)):%,%,$(filter $(strip $(1)):%,$(2)))
endef


#Include options and target specific to the OUTPUT_TYPE
include Makefile.$(OUTPUT_TYPE)

#Add all prefixes to paths
LIBS		:= $(addprefix -l,$(LIBS))
ifeq ("$(LD_SCRIPT_USR1)", "")
LD_SCRIPT	:= $(addprefix -T$(SDK_BASE)/$(SDK_LDDIR)/,$(LD_SCRIPT))
else
LD_SCRIPT_USR1	:= $(addprefix -T$(SDK_BASE)/$(SDK_LDDIR)/,$(LD_SCRIPT_USR1))
LD_SCRIPT_USR2	:= $(addprefix -T$(SDK_BASE)/$(SDK_LDDIR)/,$(LD_SCRIPT_USR2))
endif
INCDIR	:= $(addprefix -I,$(SRC_DIR))
EXTRA_INCDIR	:= $(addprefix -I,$(EXTRA_INCDIR))
MODULE_INCDIR	:= $(addsuffix /include,$(INCDIR))

ESPTOOL_FREQ=$(call maplookup,$(ESP_FLASH_FREQ_DIV),0:40m 1:26m 2:20m 0xf:80m 15:80m)
ESPTOOL_MODE=$(call maplookup,$(ESP_FLASH_MODE),0:qio 1:qout 2:dio 3:dout)
ESPTOOL_SIZE=$(call maplookup,$(ESP_SPI_FLASH_SIZE_K),512:4m 256:2m 1024:8m 2048:16m 4096:32m)

ESPTOOL_OPTS=--port $(ESPPORT) --baud $(ESPBAUD)
ESPTOOL_FLASHDEF=--flash_freq $(ESPTOOL_FREQ) --flash_mode $(ESPTOOL_MODE) --flash_size $(ESPTOOL_SIZE)

vpath %.c $(SRC_DIR)
vpath %.S $(SRC_DIR)

define compile-objects
$1/%.o: %.c
	$(vecho) "CC $$<"
	$(Q) $(CC) $(INCDIR) $(MODULE_INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS)  -c $$< -o $$@

$1/%.o: %.S
	$(vecho) "CC $$<"
	$(Q) $(CC) $(INCDIR) $(MODULE_INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS)  -c $$< -o $$@
endef

.PHONY: all checkdirs clean libesphttpd default-tgt

all: checkdirs $(TARGET_OUT) $(FW_BASE)

libesphttpd/Makefile:
	$(Q) echo "No libesphttpd submodule found. Using git to fetch it..."
	$(Q) git submodule init
	$(Q) git submodule update

libesphttpd: libesphttpd/Makefile
	$(Q) make -C libesphttpd USE_OPENSDK=$(USE_OPENSDK)

$(APP_AR): libesphttpd $(OBJ)
	$(vecho) "AR $@"
	$(Q) $(AR) cru $@ $(OBJ)

checkdirs: $(BUILD_DIR)

$(BUILD_DIR):
	$(Q) mkdir -p $@

clean:
	$(Q) make -C libesphttpd clean
	$(Q) rm -rf $(BUILD_BASE)
	
$(foreach bdir,$(BUILD_DIR),$(eval $(call compile-objects,$(bdir))))
