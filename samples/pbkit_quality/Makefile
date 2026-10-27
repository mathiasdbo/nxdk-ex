XBE_TITLE = nxdk\ sample\ -\ pbkit_quality
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c
SHADER_OBJS = ps.inl ps_tex.inl vs.inl
NXDK_DIR ?= $(CURDIR)/../..

# make CONFIG=n builds an ISO that runs only configuration n (see main.c) instead
# of cycling through all of them with pb_kill/pb_init. Run make clean when
# changing CONFIG: the object file does not depend on CFLAGS.
ifdef CONFIG
CFLAGS += -DCONFIG=$(CONFIG)
endif

include $(NXDK_DIR)/Makefile
