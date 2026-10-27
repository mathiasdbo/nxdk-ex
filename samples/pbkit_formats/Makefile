XBE_TITLE = nxdk\ sample\ -\ pbkit_formats
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
# make CONFIG=n CHAIN=1: the single configuration holds HOLD_FRAMES, then
# launches E:\Games\pbkit Formats\cfg<n+1>.xbe (or reboots after the last one),
# for a console where pb_kill/pb_init re-init does not survive. cfg0 is
# default.xbe. Logs append to E:\pbkit_formats.txt (cfg0 truncates it).
ifdef CHAIN
CFLAGS += -DCHAIN
endif
# make EXP=NO_DEPTH_READ / EXP=NO_FILE_LOG: console lock-up experiments;
# EXP=HANG spins after pb_init (watchdog test), EXP=NO_WATCHDOG leaves it out,
# EXP=GPU_ERROR tries an oversized depth clear (the console took it without an
# error), EXP=ZTILE_Z keeps pbkit's default Z tile for the depth buffer.
# The watchdog (watchdog.h) resets the console through the SMC after 10 s
# without a step or frame, and notes it in E:\pbkit_watchdog.txt.
ifdef EXP
CFLAGS += -D$(EXP)
endif

include $(NXDK_DIR)/Makefile
