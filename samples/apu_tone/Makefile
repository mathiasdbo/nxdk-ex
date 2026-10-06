XBE_TITLE = nxdk\ sample\ -\ apu_tone
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../..

SRCS = $(CURDIR)/main.c \
       $(NXDK_DIR)/lib/openal/src/apu_mem.c \
       $(NXDK_DIR)/lib/openal/src/apu_voice.c

CFLAGS += -I$(NXDK_DIR)/lib/openal/src

include $(NXDK_DIR)/Makefile
