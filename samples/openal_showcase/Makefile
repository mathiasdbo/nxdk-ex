XBE_TITLE = nxdk\ sample\ -\ openal_showcase
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../..

SRCS = $(CURDIR)/main.c \
       $(CURDIR)/showcase_input.c \
       $(CURDIR)/showcase_ui.c \
       $(CURDIR)/showcase_modes.c \
       $(CURDIR)/showcase_scene.c \
       $(CURDIR)/showcase_audio.c \
       $(CURDIR)/wav_loader.c

# Colour-passthrough shaders (same as samples/triangle) for the NV2A scene
SHADER_OBJS = ps.inl vs.inl

CFLAGS += -I$(CURDIR) -I$(NXDK_DIR)/lib/openal/include -I$(NXDK_DIR)/lib/openal/src
NXDK_SDL = y

include $(NXDK_DIR)/Makefile

# The audio mix and the scene build run on the CPU every frame: optimise just
# these two (CFLAGS itself also reaches every SDK library object)
$(CURDIR)/showcase_audio.obj $(CURDIR)/showcase_scene.obj: CFLAGS += -O2
