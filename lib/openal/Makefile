ifeq ($(NXDK_DIR),)
NXDK_DIR := $(abspath $(CURDIR)/../..)
endif

OPENAL_DIR := $(NXDK_DIR)/lib/openal

OPENAL_SRCS := \
	$(OPENAL_DIR)/src/apu_mem.c \
	$(OPENAL_DIR)/src/apu_voice.c \
	$(OPENAL_DIR)/src/apu_vp.c \
	$(OPENAL_DIR)/src/apu_hrtf.c \
	$(OPENAL_DIR)/src/apu_gp.c \
	$(OPENAL_DIR)/src/apu_ac97.c \
	$(OPENAL_DIR)/src/apu_spatial.c \
	$(OPENAL_DIR)/src/apu_eeprom.c \
	$(OPENAL_DIR)/src/apu_ep.c \
	$(OPENAL_DIR)/src/apu_voice_mgr.c \
	$(OPENAL_DIR)/src/al_buffer.c \
	$(OPENAL_DIR)/src/al_source.c \
	$(OPENAL_DIR)/src/al_listener.c \
	$(OPENAL_DIR)/src/alc_context.c

OPENAL_OBJS = $(addsuffix .obj, $(basename $(OPENAL_SRCS)))

NXDK_CFLAGS += -I$(OPENAL_DIR)/include -I$(OPENAL_DIR)/src

$(NXDK_DIR)/lib/libopenal.lib: $(OPENAL_OBJS)

main.exe: $(NXDK_DIR)/lib/libopenal.lib

CLEANRULES += clean-openal

.PHONY: clean-openal
clean-openal:
	$(VE)rm -f $(OPENAL_OBJS) $(NXDK_DIR)/lib/libopenal.lib $(OPENAL_DIR)/libopenal.a

# Standalone build support (when invoked outside top-level nxdk Makefile)
ifeq ($(LIB),)
ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin AR),default)
AR := llvm-ar
endif
CC ?= clang
AR ?= llvm-ar
CFLAGS ?= -m32 -Wall -Werror -pedantic -std=c99

.DEFAULT_GOAL := all

.PHONY: all clean
all: $(NXDK_DIR)/lib/libopenal.lib $(OPENAL_DIR)/libopenal.a

$(NXDK_DIR)/lib/libopenal.lib: $(OPENAL_OBJS)
	@echo "[ AR       ] $@"
	$(AR) rcs $@ $^

$(OPENAL_DIR)/libopenal.a: $(OPENAL_OBJS)
	@echo "[ AR       ] $@"
	$(AR) rcs $@ $^

$(OPENAL_DIR)/src/%.obj: $(OPENAL_DIR)/src/%.c
	@echo "[ CC       ] $@"
	$(CC) $(CFLAGS) -I$(OPENAL_DIR)/include -I$(OPENAL_DIR)/src -c -o $@ $<

clean: clean-openal
endif
