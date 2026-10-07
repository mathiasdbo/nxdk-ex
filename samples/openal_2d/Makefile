XBE_TITLE = nxdk\ sample\ -\ openal_2d
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../..

SRCS = $(CURDIR)/main.c \
       $(NXDK_DIR)/lib/openal/src/apu_mem.c \
       $(NXDK_DIR)/lib/openal/src/apu_voice.c \
       $(NXDK_DIR)/lib/openal/src/apu_voice_mgr.c \
       $(NXDK_DIR)/lib/openal/src/apu_spatial.c \
       $(NXDK_DIR)/lib/openal/src/apu_eeprom.c \
       $(NXDK_DIR)/lib/openal/src/apu_ep.c \
       $(NXDK_DIR)/lib/openal/src/al_buffer.c \
       $(NXDK_DIR)/lib/openal/src/al_source.c \
       $(NXDK_DIR)/lib/openal/src/al_listener.c \
       $(NXDK_DIR)/lib/openal/src/alc_context.c

CFLAGS += -I$(NXDK_DIR)/lib/openal/include -I$(NXDK_DIR)/lib/openal/src

include $(NXDK_DIR)/Makefile
