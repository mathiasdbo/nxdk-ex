# Standalone Makefile for OpenAL regression tests (host build)
#
#   make -C lib/openal/tests test    build everything and run every test
#   make -C lib/openal/tests clean   remove $(BUILD_DIR)
#
# lib/openal/src/*.c is compiled for the host into $(BUILD_DIR)/libopenal.a and
# every tests/test_*.c is linked against it. The test list is derived from the
# files on disk (see TESTS below), exactly like run_tests.py, so a new test_*.c
# can never be silently skipped. All build output goes to $(BUILD_DIR) (default:
# ./build), nothing is written next to the sources. The library is built here
# rather than through lib/openal/Makefile because the standalone mode of that
# file defaults to -m32, which cannot be linked with the host-architecture test
# programs.
ifeq ($(NXDK_DIR),)
NXDK_DIR := $(abspath $(CURDIR)/../../..)
endif

OPENAL_DIR := $(NXDK_DIR)/lib/openal
SHOWCASE_DIR := $(NXDK_DIR)/samples/openal_showcase
TESTS_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD_DIR ?= $(TESTS_DIR)/build

# GNU make predefines CC (cc) and AR (ar), so a plain "?=" would never apply.
ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin AR),default)
AR := $(shell command -v llvm-ar >/dev/null 2>&1 && echo llvm-ar || echo ar)
endif
CFLAGS ?= -Wall -Werror -pedantic -std=c99
INCLUDES := -I$(OPENAL_DIR)/include -I$(OPENAL_DIR)/src -I$(SHOWCASE_DIR)
LDLIBS ?= -lm

# Every tests/test_*.c is a test (the same discovery rule as run_tests.py).
# A test that links showcase sources needs an EXTRA_<test> entry below.
TEST_SRCS := $(sort $(wildcard $(TESTS_DIR)/test_*.c))
TESTS := $(basename $(notdir $(TEST_SRCS)))
ifeq ($(TESTS),)
$(error no test_*.c found in $(TESTS_DIR))
endif

LIB_SRCS := $(wildcard $(OPENAL_DIR)/src/*.c)
LIB_OBJS := $(patsubst $(OPENAL_DIR)/src/%.c,$(BUILD_DIR)/lib/%.o,$(LIB_SRCS))
LIBOPENAL := $(BUILD_DIR)/libopenal.a
HEADERS := $(wildcard $(OPENAL_DIR)/src/*.h $(OPENAL_DIR)/include/AL/*.h $(SHOWCASE_DIR)/*.h)

# Showcase sources that individual tests link in besides the library
EXTRA_test_wav_loader := $(SHOWCASE_DIR)/wav_loader.c
EXTRA_test_scene_engine := $(SHOWCASE_DIR)/showcase_input.c $(SHOWCASE_DIR)/showcase_ui.c
EXTRA_test_interactive_modes := $(SHOWCASE_DIR)/wav_loader.c $(SHOWCASE_DIR)/showcase_input.c \
                                $(SHOWCASE_DIR)/showcase_ui.c $(SHOWCASE_DIR)/showcase_modes.c
EXTRA_test_showcase_qa := $(EXTRA_test_interactive_modes)
EXTRA_test_showcase_scene := $(EXTRA_test_interactive_modes) $(SHOWCASE_DIR)/showcase_scene.c
EXTRA_test_showcase_audio := $(SHOWCASE_DIR)/showcase_audio.c

.PHONY: all test clean $(TESTS)
.DELETE_ON_ERROR:

all: $(addprefix $(BUILD_DIR)/,$(TESTS))

# "make test_foo" builds a single test binary
$(TESTS): %: $(BUILD_DIR)/%

test: all
	@pass=0; \
	for t in $(TESTS); do \
		echo "[ RUN      ] $$t"; \
		(cd $(TESTS_DIR) && $(BUILD_DIR)/$$t) || { echo "[  FAILED  ] $$t"; exit 1; }; \
		pass=$$((pass + 1)); \
	done; \
	echo "All $$pass of $(words $(TESTS)) tests (every tests/test_*.c) passed successfully!"

$(BUILD_DIR)/lib/%.o: $(OPENAL_DIR)/src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIBOPENAL): $(LIB_OBJS)
	$(AR) rcs $@ $^

# One rule for every test: the test source, its extra sources and the library.
# (The targets in $(TESTS) are phony aliases of these files, so make's built-in
# "%: %.c" rule never applies to them.)
$(BUILD_DIR)/test_wav_loader: $(EXTRA_test_wav_loader)
$(BUILD_DIR)/test_scene_engine: $(EXTRA_test_scene_engine)
$(BUILD_DIR)/test_interactive_modes: $(EXTRA_test_interactive_modes)
$(BUILD_DIR)/test_showcase_qa: $(EXTRA_test_showcase_qa)
$(BUILD_DIR)/test_showcase_scene: $(EXTRA_test_showcase_scene)
$(BUILD_DIR)/test_showcase_audio: $(EXTRA_test_showcase_audio)

$(BUILD_DIR)/%: $(TESTS_DIR)/%.c $(LIBOPENAL) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(EXTRA_$*) $(LIBOPENAL) $(LDLIBS) -o $@

# BUILD_DIR may be overridden; never delete the sources by accident (BUILD_DIR=.).
clean:
	@b="$(abspath $(BUILD_DIR))"; \
	case "$(TESTS_DIR)/" in "$$b"/*) echo "clean: refusing to remove $$b (it contains the test sources)" >&2; exit 1;; esac; \
	[ "$$b" != "/" ] || { echo "clean: refusing to remove /" >&2; exit 1; }; \
	rm -rf "$$b"
	rm -f $(TESTS) *.exe
