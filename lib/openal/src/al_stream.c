#include "al_stream.h"
#include "apu_mem.h"
#include "apu_voice.h"
#include <string.h>

#define RING_MASK      (AL_STREAM_RING_FRAMES - 1u)
#define RING_BYTES_MAX (AL_STREAM_RING_FRAMES * 4u)   /* stereo 16-bit */
/* Silence is kept up to this far behind the play position's next lap */
#define SILENCE_GUARD  256u

struct al_stream {
    bool in_use;
    uint8_t *ring;               /* kept in the slot when the stream is freed (reused) */
    uint32_t ring_phys;
    bool has_format;
    ALbuffer ring_buf;           /* the ring, described as a buffer for the voice layer */
    uint32_t frame_bytes;
    uint8_t silence;             /* 0 for 16-bit, 0x80 for unsigned 8-bit */
    ALbuffer *q[AL_STREAM_MAX_QUEUE];
    uint32_t q_frames[AL_STREAM_MAX_QUEUE];
    uint32_t q_n;                /* queued buffers */
    uint32_t q_done;             /* the first q_done of them are processed */
    uint32_t total;              /* frames of data in the queue */
    uint32_t read_pos;           /* stream positions, see al_stream.h */
    uint32_t write_pos;
    uint32_t silent_pos;         /* the ring is silent from write_pos up to here */
    uint32_t base;
    uint32_t last_cbo;
    uint32_t starved;
    /* Scrubbing by the output thread (al_stream_scrub_tick) */
    volatile int32_t armed;      /* the voice on `slot` plays this ring */
    uint32_t slot;
    uint32_t scrub;              /* ring index up to which played data is silenced */
};

static struct al_stream s_streams[AL_STREAM_MAX_STREAMS];

/*
 * The output thread silences ring data the voice has played, so a stream whose
 * application stops calling the library (a loading screen) plays its data and
 * then silence instead of its ring over and over. It only writes ring bytes
 * behind the play position, of armed streams; the game thread disarms a
 * stream before anything that changes its voice or ring and waits for a pass
 * in progress to finish (s_scrub_busy). Both sides store then load, so a full
 * barrier separates them.
 */
static volatile int32_t s_scrub_enabled = 1;
static volatile int32_t s_scrub_busy;

static void scrub_wait_idle(void) {
    __sync_synchronize();
    while (s_scrub_busy) {
        __sync_synchronize();
    }
}

/* a - b as a signed distance between two stream positions */
static int32_t pos_diff(uint32_t a, uint32_t b) {
    return (int32_t)(a - b);
}

static uint32_t ring_index(const al_stream_t *s, uint32_t pos) {
    return (pos - s->base) & RING_MASK;
}

void al_stream_shutdown(void) {
    uint32_t i;
    s_scrub_enabled = 0;
    scrub_wait_idle();
    for (i = 0; i < AL_STREAM_MAX_STREAMS; i++) {
        if (s_streams[i].in_use) {
            al_stream_destroy(&s_streams[i]);
        }
        if (s_streams[i].ring) {
            apu_mem_free_phys(s_streams[i].ring);
        }
        memset(&s_streams[i], 0, sizeof(s_streams[i]));
    }
}

al_stream_t *al_stream_create(void) {
    uint32_t i;
    for (i = 0; i < AL_STREAM_MAX_STREAMS; i++) {
        al_stream_t *s = &s_streams[i];
        if (!s->in_use) {
            uint8_t *ring = s->ring;
            uint32_t ring_phys = s->ring_phys;
            memset(s, 0, sizeof(*s));
            s->ring = ring;
            s->ring_phys = ring_phys;
            s->in_use = true;
            s_scrub_enabled = 1;
            return s;
        }
    }
    return NULL;
}

void al_stream_destroy(al_stream_t *s) {
    uint32_t i;
    if (!s || !s->in_use) {
        return;
    }
    al_stream_disarm(s);
    for (i = 0; i < s->q_n; i++) {
        al_buffer_release(s->q[i]);
    }
    s->q_n = 0;
    s->in_use = false;   /* the ring stays allocated for the next stream */
}

static uint32_t buffer_frame_bytes(const ALbuffer *b) {
    return (uint32_t)(b->channels * (b->bits / 8));
}

static void set_format(al_stream_t *s, const ALbuffer *b) {
    s->frame_bytes = buffer_frame_bytes(b);
    s->silence = (b->bits == 8) ? 0x80u : 0u;
    memset(&s->ring_buf, 0, sizeof(s->ring_buf));
    s->ring_buf.in_use = true;
    s->ring_buf.format = b->format;
    s->ring_buf.frequency = b->frequency;
    s->ring_buf.channels = b->channels;
    s->ring_buf.bits = b->bits;
    s->ring_buf.size = (ALsizei)(AL_STREAM_RING_FRAMES * s->frame_bytes);
    s->ring_buf.data_virt = s->ring;
    s->ring_buf.data_phys = s->ring_phys;
    s->has_format = true;
}

ALenum al_stream_queue(al_stream_t *s, ALsizei n, const ALuint *ids) {
    const ALbuffer *fmt = s->has_format ? &s->ring_buf : NULL;
    ALsizei i;

    if ((uint32_t)n > AL_STREAM_MAX_QUEUE - s->q_n) {
        return AL_INVALID_OPERATION;
    }
    /* Validate everything before queueing anything */
    for (i = 0; i < n; i++) {
        const ALbuffer *b = al_buffer_get(ids[i]);
        if (!b) {
            return AL_INVALID_NAME;
        }
        if (b->size <= 0) {
            continue;   /* no data: queued and processed as it is reached */
        }
        if (!fmt) {
            fmt = b;
        } else if (b->channels != fmt->channels || b->bits != fmt->bits || b->frequency != fmt->frequency) {
            return AL_INVALID_OPERATION;
        }
    }
    if (fmt && !s->has_format) {
        if (!s->ring) {
            s->ring = (uint8_t *)apu_mem_alloc_table(RING_BYTES_MAX, &s->ring_phys);
            if (!s->ring) {
                return AL_OUT_OF_MEMORY;
            }
        }
        set_format(s, fmt);
    }
    for (i = 0; i < n; i++) {
        ALbuffer *b = al_buffer_get(ids[i]);
        al_buffer_retain(b);
        s->q[s->q_n] = b;
        s->q_frames[s->q_n] = (b->size > 0) ? (uint32_t)b->size / buffer_frame_bytes(b) : 0u;
        s->total += s->q_frames[s->q_n];
        s->q_n++;
    }
    return AL_NO_ERROR;
}

static uint32_t sub_floor0(uint32_t pos, uint32_t n) {
    return (pos_diff(pos, n) < 0) ? 0u : pos - n;
}

ALenum al_stream_unqueue(al_stream_t *s, ALsizei n, ALuint *ids) {
    uint32_t i, removed = 0;

    if ((uint32_t)n > s->q_done) {
        return AL_INVALID_VALUE;
    }
    for (i = 0; i < (uint32_t)n; i++) {
        ids[i] = s->q[i]->id;
        al_buffer_release(s->q[i]);
        removed += s->q_frames[i];
    }
    memmove(&s->q[0], &s->q[n], (s->q_n - (uint32_t)n) * sizeof(s->q[0]));
    memmove(&s->q_frames[0], &s->q_frames[n], (s->q_n - (uint32_t)n) * sizeof(s->q_frames[0]));
    s->q_n -= (uint32_t)n;
    s->q_done -= (uint32_t)n;
    /* Positions count from the start of the queue: shift them with it. After a
     * stop, the read position may be inside the removed data; the next play
     * rewinds anyway. The ring keeps its content: base moves with the positions. */
    s->total -= removed;
    s->read_pos = sub_floor0(s->read_pos, removed);
    s->write_pos = sub_floor0(s->write_pos, removed);
    s->silent_pos = sub_floor0(s->silent_pos, removed);
    s->base -= removed;
    if (s->q_n == 0) {
        s->has_format = false;   /* an empty queue takes any format */
    }
    return AL_NO_ERROR;
}

uint32_t al_stream_queued(const al_stream_t *s) {
    return s->q_n;
}

uint32_t al_stream_processed(const al_stream_t *s) {
    return s->q_done;
}

uint32_t al_stream_total_frames(const al_stream_t *s) {
    return s->total;
}

const ALbuffer *al_stream_ring_buffer(const al_stream_t *s) {
    return s->has_format ? &s->ring_buf : NULL;
}

/* Queue entry holding stream position pos (looping: pos modulo the data), and the offset in it */
static int find_entry(const al_stream_t *s, uint32_t pos, bool looping, uint32_t *offset) {
    uint32_t i, start = 0;
    if (looping) {
        if (s->total == 0) {
            return -1;
        }
        pos %= s->total;
    }
    for (i = 0; i < s->q_n; i++) {
        if (pos < start + s->q_frames[i]) {
            *offset = pos - start;
            return (int)i;
        }
        start += s->q_frames[i];
    }
    return -1;
}

ALuint al_stream_current_buffer(const al_stream_t *s) {
    uint32_t off;
    int e;
    if (s->q_n == 0) {
        return 0;
    }
    e = find_entry(s, s->read_pos, true, &off);
    if (e < 0 || (pos_diff(s->read_pos, s->total) >= 0)) {
        return s->q[s->q_n - 1]->id;
    }
    return s->q[e]->id;
}

void al_stream_rewind(al_stream_t *s) {
    s->read_pos = s->write_pos = s->silent_pos = s->base = 0;
    s->last_cbo = 0;
    s->q_done = 0;
}

void al_stream_finish(al_stream_t *s) {
    s->q_done = s->q_n;
}

static void ring_put(al_stream_t *s, uint32_t pos, const uint8_t *src, uint32_t frames) {
    /* src NULL: silence */
    while (frames > 0) {
        uint32_t idx = ring_index(s, pos);
        uint32_t n = AL_STREAM_RING_FRAMES - idx;
        if (n > frames) {
            n = frames;
        }
        if (src) {
            memcpy(s->ring + idx * s->frame_bytes, src, n * s->frame_bytes);
            src += n * s->frame_bytes;
        } else {
            memset(s->ring + idx * s->frame_bytes, s->silence, n * s->frame_bytes);
        }
        pos += n;
        frames -= n;
    }
}

static void fill(al_stream_t *s, bool looping) {
    uint32_t target = s->read_pos + AL_STREAM_LEAD_FRAMES;
    uint32_t quiet_to = s->read_pos + AL_STREAM_RING_FRAMES - SILENCE_GUARD;

    if (!s->has_format || !s->ring) {
        return;
    }
    /* Data, at most the lead ahead of the play position */
    while (pos_diff(target, s->write_pos) > 0) {
        uint32_t off, n;
        int e = find_entry(s, s->write_pos, looping, &off);
        if (e < 0) {
            break;   /* past the end of the queued data */
        }
        n = s->q_frames[e] - off;
        if (n > (uint32_t)pos_diff(target, s->write_pos)) {
            n = (uint32_t)pos_diff(target, s->write_pos);
        }
        ring_put(s, s->write_pos, (const uint8_t *)s->q[e]->data_virt + off * s->frame_bytes, n);
        s->write_pos += n;
    }
    /* Silence behind it, up to the next lap of the play position */
    if (pos_diff(s->silent_pos, s->write_pos) < 0) {
        s->silent_pos = s->write_pos;
    }
    if (pos_diff(quiet_to, s->silent_pos) > 0) {
        ring_put(s, s->silent_pos, NULL, (uint32_t)pos_diff(quiet_to, s->silent_pos));
        s->silent_pos = quiet_to;
    }
}

void al_stream_voice_start(al_stream_t *s, bool looping) {
    s->base = s->read_pos;
    s->last_cbo = 0;
    s->write_pos = s->silent_pos = s->read_pos;
    if (s->has_format && s->ring) {
        memset(s->ring, s->silence, AL_STREAM_RING_FRAMES * s->frame_bytes);
    }
    fill(s, looping);
}

static void update_processed(al_stream_t *s) {
    uint32_t i, end = 0, done = 0;
    for (i = 0; i < s->q_n; i++) {
        end += s->q_frames[i];
        if (pos_diff(s->read_pos, end) < 0) {
            break;
        }
        done = i + 1u;
    }
    if (done > s->q_done) {
        s->q_done = done;
    }
}

bool al_stream_update(al_stream_t *s, uint32_t ring_pos, bool looping) {
    ring_pos &= RING_MASK;
    s->read_pos += (ring_pos - s->last_cbo) & RING_MASK;
    s->last_cbo = ring_pos;

    /* The voice got past the data written for it: that stretch played as
     * silence, and the data for it is skipped (time has moved on) */
    if (pos_diff(s->read_pos, s->write_pos) > 0) {
        if (looping || pos_diff(s->write_pos, s->total) < 0) {
            s->starved++;
        }
        s->write_pos = s->read_pos;
    }
    fill(s, looping);
    if (looping) {
        /* Keep the positions in the current lap, so that switching looping off
         * ends the stream at the end of this lap */
        if (s->total > 0 && pos_diff(s->read_pos, s->total) >= 0) {
            uint32_t laps = (s->read_pos / s->total) * s->total;
            s->read_pos -= laps;
            s->write_pos -= laps;
            s->silent_pos -= laps;
            s->base -= laps;
        }
        return false;
    }
    update_processed(s);
    return pos_diff(s->read_pos, s->total) >= 0;
}

uint32_t al_stream_starved(const al_stream_t *s) {
    return s->starved;
}

void al_stream_arm(al_stream_t *s, uint32_t slot) {
    if (!s) {
        return;
    }
    s->slot = slot;
    s->scrub = 0;   /* the voice starts at ring index 0 */
    __sync_synchronize();
    s->armed = 1;
}

void al_stream_disarm(al_stream_t *s) {
    if (!s || !s->armed) {
        return;
    }
    s->armed = 0;
    scrub_wait_idle();
}

void al_stream_scrub_tick(void) {
    uint32_t i;
    s_scrub_busy = 1;
    __sync_synchronize();
    if (s_scrub_enabled) {
        for (i = 0; i < AL_STREAM_MAX_STREAMS; i++) {
            al_stream_t *s = &s_streams[i];
            uint32_t pos, n;
            if (!s->in_use || !s->armed || !s->has_format || !s->ring) {
                continue;
            }
            pos = apu_voice_get_position(s->slot) & RING_MASK;
            n = (pos - s->scrub) & RING_MASK;
            if (n >= AL_STREAM_RING_FRAMES / 2u) {
                /* not plausible for a 4 ms pass: leave it, take the new position */
                s->scrub = pos;
                continue;
            }
            while (n > 0) {
                uint32_t run = AL_STREAM_RING_FRAMES - s->scrub;
                if (run > n) {
                    run = n;
                }
                memset(s->ring + s->scrub * s->frame_bytes, s->silence, run * s->frame_bytes);
                s->scrub = (s->scrub + run) & RING_MASK;
                n -= run;
            }
        }
    }
    __sync_synchronize();
    s_scrub_busy = 0;
}
