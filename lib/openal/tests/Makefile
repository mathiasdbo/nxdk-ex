# Standalone Makefile for OpenAL regression tests
ifeq ($(NXDK_DIR),)
NXDK_DIR := $(abspath $(CURDIR)/../../..)
endif

OPENAL_DIR := $(NXDK_DIR)/lib/openal
CC ?= clang
CFLAGS ?= -I$(OPENAL_DIR)/include -I$(OPENAL_DIR)/src -Wall -Werror -pedantic -std=c99
LDFLAGS ?= -L$(OPENAL_DIR) -lopenal -lm

TESTS = test_alc_context \
        test_benchmark \
        test_buffer_prd \
        test_dse_toslink \
        test_eeprom_smbus \
        test_hrtf_biquad \
        test_itd_woodworth \
        test_lib_build \
        test_listener_math \
        test_panning_51 \
        test_pitch_gain \
        test_preemption \
        test_source_2d \
        test_spatial_attenuation \
        test_stereo_downmix \
        test_surround_validation \
        test_voice_mgr \
        test_wav_loader \
        test_scene_engine \
        test_interactive_modes

.PHONY: all test clean $(TESTS)

all: $(OPENAL_DIR)/libopenal.a $(TESTS)

$(OPENAL_DIR)/libopenal.a:
	$(MAKE) -C $(OPENAL_DIR)

test: all
	@for t in $(TESTS); do \
		echo "[ RUN      ] $$t"; \
		./$$t || exit 1; \
	done
	@echo "All 20 tests passed successfully!"

test_wav_loader: test_wav_loader.c $(OPENAL_DIR)/libopenal.a $(NXDK_DIR)/samples/openal_showcase/wav_loader.c
	$(CC) $(CFLAGS) -I$(NXDK_DIR)/samples/openal_showcase test_wav_loader.c $(NXDK_DIR)/samples/openal_showcase/wav_loader.c $(OPENAL_DIR)/libopenal.a -lm -o $@

test_scene_engine: test_scene_engine.c $(OPENAL_DIR)/libopenal.a $(NXDK_DIR)/samples/openal_showcase/showcase_input.c $(NXDK_DIR)/samples/openal_showcase/showcase_ui.c
	$(CC) $(CFLAGS) -I$(NXDK_DIR)/samples/openal_showcase test_scene_engine.c $(NXDK_DIR)/samples/openal_showcase/showcase_input.c $(NXDK_DIR)/samples/openal_showcase/showcase_ui.c $(OPENAL_DIR)/libopenal.a -lm -o $@

test_interactive_modes: test_interactive_modes.c $(OPENAL_DIR)/libopenal.a $(NXDK_DIR)/samples/openal_showcase/wav_loader.c $(NXDK_DIR)/samples/openal_showcase/showcase_input.c $(NXDK_DIR)/samples/openal_showcase/showcase_ui.c $(NXDK_DIR)/samples/openal_showcase/showcase_modes.c
	$(CC) $(CFLAGS) -I$(NXDK_DIR)/samples/openal_showcase test_interactive_modes.c $(NXDK_DIR)/samples/openal_showcase/wav_loader.c $(NXDK_DIR)/samples/openal_showcase/showcase_input.c $(NXDK_DIR)/samples/openal_showcase/showcase_ui.c $(NXDK_DIR)/samples/openal_showcase/showcase_modes.c $(OPENAL_DIR)/libopenal.a -lm -o $@

%: %.c $(OPENAL_DIR)/libopenal.a
	$(CC) $(CFLAGS) $< $(OPENAL_DIR)/libopenal.a -lm -o $@

clean:
	rm -f $(TESTS) *.exe
