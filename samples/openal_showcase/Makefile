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

# Audio backend:
#   apu (default) - the OpenAL library is compiled into the sample for the real
#                   MCPX APU (each apu/*.c includes one library source and only
#                   those objects get -DOPENAL_APU_REAL_MMIO, like samples/openal_vp);
#                   if the APU does not come up, the CPU mixer takes over
#   cpu           - shared lib/libopenal.lib (null backend) + the CPU mixer only
SHOWCASE_BACKEND ?= apu
ifeq ($(SHOWCASE_BACKEND),apu)
APU_SRCS = $(wildcard $(CURDIR)/apu/*.c)
SRCS += $(APU_SRCS)
endif

include $(NXDK_DIR)/Makefile

ifeq ($(SHOWCASE_BACKEND),apu)
APU_OBJS = $(APU_SRCS:.c=.obj)
$(APU_OBJS): CFLAGS += -DOPENAL_APU_REAL_MMIO
# Rebuild a wrapper when the library source it includes (or any library header) changes
$(APU_OBJS): $(CURDIR)/apu/%.obj: $(NXDK_DIR)/lib/openal/src/%.c $(wildcard $(NXDK_DIR)/lib/openal/src/*.h)
endif

# The audio mix and the scene build run on the CPU every frame: optimise just
# these two (CFLAGS itself also reaches every SDK library object)
$(CURDIR)/showcase_audio.obj $(CURDIR)/showcase_scene.obj: CFLAGS += -O2
