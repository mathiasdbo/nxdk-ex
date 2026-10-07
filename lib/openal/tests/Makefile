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
        test_voice_mgr

.PHONY: all test clean $(TESTS)

all: $(OPENAL_DIR)/libopenal.a $(TESTS)

$(OPENAL_DIR)/libopenal.a:
	$(MAKE) -C $(OPENAL_DIR)

test: all
	@for t in $(TESTS); do \
		echo "[ RUN      ] $$t"; \
		./$$t || exit 1; \
	done
	@echo "All 17 tests passed successfully!"

%: %.c $(OPENAL_DIR)/libopenal.a
	$(CC) $(CFLAGS) $< $(OPENAL_DIR)/libopenal.a -lm -o $@

clean:
	rm -f $(TESTS) *.exe
