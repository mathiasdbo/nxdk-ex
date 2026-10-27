XBE_TITLE = nxdk\ sample\ -\ pbkit_display
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c
SHADER_OBJS = ps.inl vs.inl
NXDK_DIR ?= $(CURDIR)/../..

# make CONFIG=n builds an ISO that runs only configuration n (see main.c) instead
# of cycling through all of them with pb_kill/pb_init. Run make clean when
# changing CONFIG: the object file does not depend on CFLAGS.
ifdef CONFIG
CFLAGS += -DCONFIG=$(CONFIG)
endif
# make CONFIG=1 OV_UYVY=1: the overlay surface in UYVY instead of YUY2 (xemu supports YUY2 only)
ifdef OV_UYVY
CFLAGS += -DOV_UYVY
endif

include $(NXDK_DIR)/Makefile
