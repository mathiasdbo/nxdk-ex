XBE_TITLE = nxdk\ sample\ -\ openal_showcase
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../..

SRCS = $(CURDIR)/main.c \
       $(CURDIR)/showcase_input.c \
       $(CURDIR)/showcase_ui.c \
       $(CURDIR)/showcase_modes.c \
       $(CURDIR)/wav_loader.c

CFLAGS += -I$(CURDIR) -I$(NXDK_DIR)/lib/openal/include -I$(NXDK_DIR)/lib/openal/src
NXDK_SDL = y

include $(NXDK_DIR)/Makefile
