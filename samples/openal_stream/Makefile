XBE_TITLE = nxdk\ sample\ -\ openal_stream
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/../..

# The OpenAL library is compiled into this sample for the real APU: each
# mmio/*.c includes one library source, and only those objects get
# -DOPENAL_APU_REAL_MMIO, so the shared lib/libopenal.lib (null backend) is
# left as it is. They come first on the link line, so no libopenal.lib member
# is pulled in.
MMIO_SRCS = $(wildcard $(CURDIR)/mmio/*.c)

SRCS = $(CURDIR)/main.c $(MMIO_SRCS)

CFLAGS += -I$(NXDK_DIR)/lib/openal/include -I$(NXDK_DIR)/lib/openal/src

include $(NXDK_DIR)/Makefile

MMIO_OBJS = $(MMIO_SRCS:.c=.obj)
$(MMIO_OBJS): CFLAGS += -DOPENAL_APU_REAL_MMIO
# Rebuild a wrapper when the library source it includes (or any library header) changes
$(MMIO_OBJS): $(CURDIR)/mmio/%.obj: $(NXDK_DIR)/lib/openal/src/%.c $(wildcard $(NXDK_DIR)/lib/openal/src/*.h)
