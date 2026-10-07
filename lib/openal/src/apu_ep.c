#include "apu_ep.h"

int apu_ep_subsystem_init(uintptr_t apu_base, APU_AUDIO_TOPOLOGY topology) {
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    if (topology == APU_TOPOLOGY_SURROUND_51) {
        apu_write32(apu_base, NV_PAPU_EP_FIFO_CONFIG, NV_PAPU_EP_FIFO_CONFIG_SURROUND);
        apu_write32(apu_base, NV_PAPU_EP_FIFO_ROUTE, NV_PAPU_EP_ROUTE_DEFAULT);
        apu_write32(apu_base, NV_PAPU_EP_CONTROL, NV_PAPU_EP_CONTROL_ENABLE | NV_PAPU_EP_CONTROL_DSE_ENABLE);
        return 0;
    } else if (topology == APU_TOPOLOGY_STEREO_20) {
        apu_write32(apu_base, NV_PAPU_EP_FIFO_CONFIG, NV_PAPU_EP_FIFO_CONFIG_STEREO);
        apu_write32(apu_base, NV_PAPU_EP_FIFO_ROUTE, NV_PAPU_EP_ROUTE_DEFAULT);
        apu_write32(apu_base, NV_PAPU_EP_CONTROL, NV_PAPU_EP_CONTROL_ENABLE);
        return 0;
    }

    return -1;
}

void apu_ep_subsystem_deinit(uintptr_t apu_base) {
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    apu_write32(apu_base, NV_PAPU_EP_CONTROL, 0u);
}

bool apu_is_dolby_digital_active(uintptr_t apu_base) {
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    uint32_t ctrl = apu_read32(apu_base, NV_PAPU_EP_CONTROL);
    return (ctrl & NV_PAPU_EP_CONTROL_DSE_ENABLE) != 0;
}

uint32_t apu_ep_get_fifo_config(uintptr_t apu_base) {
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    return apu_read32(apu_base, NV_PAPU_EP_FIFO_CONFIG);
}

uint32_t apu_ep_get_fifo_route(uintptr_t apu_base) {
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    return apu_read32(apu_base, NV_PAPU_EP_FIFO_ROUTE);
}
