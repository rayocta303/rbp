/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMS_LOADER_H
#define RX3_STEMS_LOADER_H

#include "rx3_stems_package.h"

/* Loads run as jobs on the core's shared loader. The pending slot per deck
   keeps only the newest request; a job takes whatever is pending. */
static volatile unsigned int stems_queue_lock;
static struct stems_load_request *stems_pending_loads[2];

static void stems_lock(void)
{
    while (__atomic_exchange_n(&stems_queue_lock, 1u, __ATOMIC_SEQ_CST)) usleep(1000u);
}

static void stems_unlock(void)
{
    __atomic_store_n(&stems_queue_lock, 0u, __ATOMIC_SEQ_CST);
}

static void stems_drain(struct stems_deck_context *context)
{
    for (;;) {
        while (__atomic_load_n(&context->readers_active, __ATOMIC_SEQ_CST)) usleep(1000u);
        usleep(10000u);
        if (!__atomic_load_n(&context->readers_active, __ATOMIC_SEQ_CST)) return;
    }
}

static void stems_release_request(struct stems_load_request *request)
{
    if (!request) return;
    for (unsigned int i = 0; i < 3u; i++) if (request->fds[i] >= 0) close(request->fds[i]);
    munmap(request, 4096u);
}

static int stems_load_payload(int fd, struct stem_payload *destination,
                               unsigned int other_bytes, const struct stems_io *io)
{
    struct stem_header header;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < (off_t)sizeof(header) || lseek(fd, 0, SEEK_SET) != 0 ||
        stems_read(fd, &header, sizeof(header), io)) return 0;
    if (!stems_pcm_gain(&header) || memcmp(header.magic, "RX3STM1\0", 8u) ||
        header.sample_rate != 44100u || header.channels != 2u || header.header_size != 64u ||
        !header.frames || header.frames > RX3_STEMS_MAX_FRAMES ||
        header.frames * 4u + 64u != (uint64_t)size) return 0;
    size_t bytes = (size_t)header.frames * 4u;
    if (other_bytes >= RX3_STEMS_RESIDENT_BYTES || bytes > RX3_STEMS_RESIDENT_BYTES - other_bytes)
        return 0;
    /* The player keeps its reserve; the shared ledger counts what other
       modules are allocating at this moment. */
    const struct rx3_memory_service *memory = framework->memory;
    if (!memory->reserve(&stems_decks, bytes, RX3_STEMS_PLAYER_RESERVE_KIB)) return 0;
    void *block = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
        memory->abandon(&stems_decks, bytes);
        return 0;
    }
    if (stems_read(fd, block, bytes, io) || mprotect(block, bytes, PROT_READ)) {
        munmap(block, bytes);
        memory->abandon(&stems_decks, bytes);
        return 0;
    }
    memory->settle(&stems_decks, bytes);
    destination->data = block;
    destination->format = header.format;
    destination->pcm_gain = stems_pcm_gain(&header);
    destination->frames = header.frames;
    destination->block = block;
    destination->block_size = bytes;
    return 1;
}

/* The worker is single-threaded. The other deck can release memory while
   this set loads, but cannot allocate a second pending set concurrently. */
static unsigned int stems_load_set(const int fds[3], struct stem_payload next[3],
                                    unsigned int other_bytes, const struct stems_io *io)
{
    unsigned int count = 0u;
    while (count < 3u && fds[count] >= 0) count++;
    unsigned int resident = other_bytes;
    for (unsigned int i = 0; i < count; i++) {
        if (!stems_load_payload(fds[i], &next[i], resident, io) ||
            (i && next[i].frames != next[0].frames)) {
            release_payload(&next[i]);
            return i; /* Preserve the validated prefix instead of reading it again. */
        }
        resident += next[i].block_size;
    }
    return count;
}

static int stems_loader_stopping(void)
{
    return framework->loader->stopping(&stems_decks);
}

static int stems_request_cancelled(const void *opaque)
{
    const struct stems_load_request *request = opaque;
    return stems_loader_stopping() ||
        __atomic_load_n(&request->context->generation, __ATOMIC_SEQ_CST) != request->generation;
}

/* Puts a validated set on the deck, under the queue lock, if the request
   still owns it. On success `next` is emptied (the context owns it now). */
static int stems_publish(struct stems_load_request *request, struct stem_payload next[3],
                         unsigned int count, unsigned int wanted, unsigned int loaded)
{
    struct stems_deck_context *context = request->context;
    int published = 0;
    stems_lock();
    if (!stems_request_cancelled(request) && context->reader == request->reader) {
        if (count) {
            __atomic_store_n(&context->reader, 0, __ATOMIC_SEQ_CST);
            stems_drain(context);
            for (unsigned int i = 0; i < 3u; i++) release_payload(&context->payloads[i]);
            memcpy(context->payloads, next, sizeof(context->payloads));
            memset(next, 0, sizeof(context->payloads));
            context->payload_count = count;
            __atomic_store_n(&context->loaded, loaded == ~0u ? 0u : loaded, __ATOMIC_RELEASE);
            __atomic_store_n(&context->streaming, loaded != ~0u, __ATOMIC_RELEASE);
            unsigned int available = (2u << count) - 1u;
            unsigned int old = __atomic_load_n(&context->selection, __ATOMIC_SEQ_CST);
            __atomic_store_n(&context->selection,
                              ((old & ~255u) + 256u) | available * 17u, __ATOMIC_SEQ_CST);
            stems_set_mask(context, available);
            stems_iso_track_ready((unsigned int)(context - stems_decks));
            __atomic_store_n(&context->reader, request->reader, __ATOMIC_SEQ_CST);
            __atomic_store_n(&context->status, 2u, __ATOMIC_SEQ_CST);
            log_number("stems ready, available roles = ", count + 1u);
            if (count < wanted) log_line("stems reduced: an additional stem was rejected");
            published = 1;
        } else {
            __atomic_store_n(&context->armed, 0, __ATOMIC_SEQ_CST);
            __atomic_store_n(&context->status, 3u, __ATOMIC_SEQ_CST);
            log_line("stems rejected: no valid resident set");
        }
    }
    stems_unlock();
    return published;
}

/* A package streamed in (RX3, 2026-10-15: a four-part track took 21 s to
 * appear, read whole from the drive before use). Header, entries, the parts'
 * headers and the waveform first; then the set is published with nothing
 * resident, and the parts are read side by side from the start of the track,
 * a second and a half at a time, so the stems play from the beginning within
 * moments while the rest arrives. Each piece is read into a buffer of the
 * job's own and copied into the set under the queue lock, the lock every
 * release takes, so a track change never frees memory being written. The
 * CRC of every part is still checked, piece by piece; a damaged package ends
 * with nothing resident, which is stock audio. */
#define STEMS_STREAM_FRAMES 65536u
static unsigned int stems_armed_ms[2];   /* when the deck handed its stems to the loader */

static void stems_package_stream(struct stems_load_request *request, int fd,
                                 unsigned int other_bytes, const struct stems_io *io)
{
    struct stems_deck_context *context = request->context;
    struct stem_payload next[3] = {0};
    struct rx3_package_header h;
    unsigned int deck_index = (unsigned int)(context - stems_decks) & 1u;
    unsigned int started_ms = now_ms();
    log_number("stems timing: queued before the read, ms = ", started_ms - stems_armed_ms[deck_index]);
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 64 || lseek(fd, 0, SEEK_SET) != 0 || stems_read(fd, &h, 64u, io) ||
        memcmp(h.magic, "RX3PKG2\0", 8u) || (h.version != 2 && h.version != 3) ||
        h.header_size != 64 || h.entry_size != 64 || h.count < 3 || h.count > 5 ||
        h.size != (uint64_t)size || h.frames < RX3_STEMS_MIN_FRAMES ||
        h.frames > RX3_STEMS_MAX_FRAMES || !package_zero(h.reserved, 24) ||
        (uint64_t)size > RX3_STEMS_RESIDENT_BYTES) {
        stems_publish(request, next, 0u, 0u, 0u);
        return;
    }
    unsigned int bytes = (unsigned int)size, roles = h.count - 2u;
    if (other_bytes >= RX3_STEMS_RESIDENT_BYTES || bytes > RX3_STEMS_RESIDENT_BYTES - other_bytes) {
        stems_publish(request, next, 0u, 0u, 0u);
        return;
    }
    const struct rx3_memory_service *memory = framework->memory;
    if (!memory->reserve(&stems_decks, bytes, RX3_STEMS_PLAYER_RESERVE_KIB)) {
        stems_publish(request, next, 0u, 0u, 0u);
        return;
    }
    uint8_t *block = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *bounce = mmap(0, STEMS_STREAM_FRAMES * 4u, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED || bounce == MAP_FAILED) {
        if (block != MAP_FAILED) munmap(block, bytes);
        if (bounce != MAP_FAILED) munmap(bounce, STEMS_STREAM_FRAMES * 4u);
        memory->abandon(&stems_decks, bytes);
        stems_publish(request, next, 0u, 0u, 0u);
        return;
    }
    memcpy(block, &h, 64u);
    unsigned int offset = 64u + h.count * 64u;
    const struct rx3_package_entry *entries = (const void *)(block + 64u);
    const uint8_t *wave = 0;
    static const char *names[5] = {"vocals", "drums", "bass", "waveform", "manifest"};
    int good = offset <= bytes && !stems_read(fd, block + 64u, h.count * 64u, io);
    for (unsigned int i = 0; good && i < h.count; i++) {
        const struct rx3_package_entry *e = &entries[i];
        unsigned int kind = i < roles ? 1u : i == roles ? 2u : 3u;
        unsigned int role = i < roles ? 2u << i : 0u;
        const char *name = i < roles ? names[i] : names[3u + i - roles];
        unsigned int len = 0; while (name[len]) len++;
        if (e->kind != kind || e->role != role || e->offset != offset || e->reserved ||
            (!e->size && !(h.version == 3 && kind == 2u)) || e->size > bytes - offset ||
            memcmp(e->name, name, len) || !package_zero((const uint8_t *)e->name + len, 32u - len) ||
            (kind == 3u && e->size > RX3_STEMS_MANIFEST_BYTES) ||
            (kind == 1u && e->size != 64u + h.frames * 4u)) { good = 0; break; }
        /* A part's 64-byte header now; whole sections for the rest. */
        unsigned int head = kind == 1u ? 64u : (unsigned int)e->size;
        if (head && (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset ||
                     stems_read(fd, block + offset, head, io))) { good = 0; break; }
        if (kind == 1u) {
            const struct stem_header *pcm = (const void *)(block + offset);
            if (memcmp(pcm->magic, "RX3STM1\0", 8) || pcm->sample_rate != 44100 ||
                pcm->channels != 2 || !stems_pcm_gain(pcm) || pcm->header_size != 64 ||
                pcm->frames != h.frames) { good = 0; break; }
        } else {
            uint32_t crc;
            if (!package_crc(block + offset, (unsigned int)e->size, io, &crc) || crc != e->crc) {
                good = 0; break;
            }
        }
        offset += (unsigned int)e->size;
    }
    good = good && offset == bytes && package_wave(block, &entries[roles], &h, roles, io, &wave);
    if (!good) {
        munmap(bounce, STEMS_STREAM_FRAMES * 4u);
        munmap(block, bytes);
        memory->abandon(&stems_decks, bytes);
        stems_publish(request, next, 0u, 0u, 0u);
        return;
    }
    memory->settle(&stems_decks, bytes);
    for (unsigned int i = 0; i < roles; i++) {
        const struct stem_header *pcm = (const void *)(block + (unsigned int)entries[i].offset);
        next[i].data = block + (unsigned int)entries[i].offset + 64u;
        next[i].format = pcm->format; next[i].pcm_gain = stems_pcm_gain(pcm); next[i].frames = h.frames;
    }
    next[0].block = block; next[0].block_size = bytes;
    next[0].wave = wave; next[0].wave_size = (unsigned int)entries[roles].size;
    log_line("stems package: streaming");
    log_number("stems timing: header and waveforms read, ms = ", now_ms() - started_ms);
    if (!stems_publish(request, next, roles, roles, 0u)) {
        for (unsigned int i = 0; i < 3u; i++) release_payload(&next[i]);
        munmap(bounce, STEMS_STREAM_FRAMES * 4u);
        return;
    }
    /* From here the set belongs to the deck; touch it only under the lock and
       only while it is still the deck's. */
    package_crc_prepare();
    uint32_t crc[3];
    for (unsigned int r = 0; r < roles; r++)
        crc[r] = package_crc_update(~0u, block + (unsigned int)entries[r].offset, 64u);
    unsigned int frames = (unsigned int)h.frames, ours = 1u;
    for (unsigned int start = 0; ours && start < frames; ) {
        unsigned int count = frames - start > STEMS_STREAM_FRAMES ? STEMS_STREAM_FRAMES : frames - start;
        for (unsigned int r = 0; ours && r < roles; r++) {
            unsigned int at = (unsigned int)entries[r].offset + 64u + start * 4u;
            if (lseek(fd, (off_t)at, SEEK_SET) != (off_t)at || stems_read(fd, bounce, count * 4u, io)) {
                ours = 0u; break;
            }
            crc[r] = package_crc_update(crc[r], bounce, count * 4u);
            stems_lock();
            ours = !stems_request_cancelled(request) && context->payloads[0].block == block;
            if (ours) {
                memcpy(block + at, bounce, count * 4u);
                if (r + 1u == roles)
                    __atomic_store_n(&context->loaded, start + count, __ATOMIC_RELEASE);
            }
            stems_unlock();
        }
        start += count;
    }
    munmap(bounce, STEMS_STREAM_FRAMES * 4u);
    stems_lock();
    if (context->payloads[0].block == block && !stems_request_cancelled(request)) {
        int whole = 1;
        for (unsigned int r = 0; r < roles; r++) whole &= ~crc[r] == entries[r].crc;
        if (whole && __atomic_load_n(&context->loaded, __ATOMIC_ACQUIRE) == frames) {
            (void)mprotect(block, bytes, PROT_READ);
            __atomic_store_n(&context->streaming, 0u, __ATOMIC_RELEASE);
            log_line("stems package: streamed and verified");
            log_number("stems timing: whole package, ms = ", now_ms() - started_ms);
        } else {
            __atomic_store_n(&context->loaded, 0u, __ATOMIC_RELEASE);
            log_line("stems package: damaged or cut short, the deck plays stock");
        }
    }
    stems_unlock();
}

/* A loader job: every pending request, newest per deck, then return. */
static void stems_load_pending(void *unused)
{
    (void)unused;
    while (!stems_loader_stopping()) {
        struct stems_load_request *request = 0;
        stems_lock();
        for (unsigned int deck = 0; deck < 2u; deck++)
            if (stems_pending_loads[deck]) {
                request = stems_pending_loads[deck];
                stems_pending_loads[deck] = 0;
                break;
            }
        stems_unlock();
        if (!request) return;
        struct stems_deck_context *context = request->context;
        const struct stems_io io = {stems_request_cancelled, request};
        struct stem_payload next[3] = {0};
        unsigned int count = 0u;
        while (count < 3u && request->fds[count] >= 0) count++;
        unsigned int wanted = count;
        stems_lock();
        struct stems_deck_context *other = &stems_decks[context == &stems_decks[0] ? 1u : 0u];
        unsigned int other_bytes = 0u;
        for (unsigned int j = 0; j < other->payload_count; j++)
            other_bytes += other->payloads[j].block_size;
        stems_unlock();
        char magic[8];
        int packaged = lseek(request->fds[0], 0, SEEK_SET) == 0 &&
            !stems_read(request->fds[0], magic, 8u, &io) && !memcmp(magic, "RX3PKG2\0", 8u);
        /* A package is one file holding up to three parts: show every pad
           from its header at once, instead of VOCAL/INST until the whole file
           is read (RX3, 2026-10-14: four-part tracks showed two pads). */
        struct rx3_package_header early;
        if (packaged && lseek(request->fds[0], 0, SEEK_SET) == 0 &&
            !stems_read(request->fds[0], &early, 64u, &io) &&
            early.count >= 3u && early.count <= 5u) {
            unsigned int shown = (2u << (early.count - 2u)) - 1u;
            stems_lock();
            if (!stems_request_cancelled(request) && context->reader == request->reader &&
                __atomic_load_n(&context->status, __ATOMIC_SEQ_CST) == 1u) {
                unsigned int old = __atomic_load_n(&context->selection, __ATOMIC_SEQ_CST);
                __atomic_store_n(&context->selection, (old & ~255u) | shown * 17u, __ATOMIC_SEQ_CST);
                stems_set_mask(context, shown);
            }
            stems_unlock();
        }
        if (packaged && !stems_io_cancelled(&io)) {
            stems_package_stream(request, request->fds[0], other_bytes, &io);
            stems_release_request(request);
            continue;
        }
        count = stems_io_cancelled(&io) ? 0u : stems_load_set(request->fds, next, other_bytes, &io);
        (void)stems_publish(request, next, count, wanted, ~0u);
        for (unsigned int i = 0; i < 3u; i++) release_payload(&next[i]);
        stems_release_request(request);
    }
}

/* A new track starts with empty echo lines and no tail. Load thread, with
   the deck's reader cleared and its audio drained. */
static void stems_echo_flush(unsigned int deck)
{
    struct stems_echo_deck *echo = &stems_echo[deck];
    __atomic_store_n(&echo->tails, 0u, __ATOMIC_SEQ_CST);
    if (echo->buffer[0])
        for (unsigned int i = 0; i < 4u; i++)
            memset(echo->buffer[i], 0, (size_t)STEMS_ECHO_FRAMES * sizeof(Short2));
    memset(echo->line, 0, sizeof(echo->line));
    echo->cursor = echo->window = echo->running = 0u;
    struct stems_echo_deck *alt = &stems_echo_alt[deck];
    if (alt->buffer[0])
        for (unsigned int i = 0; i < 4u; i++)
            memset(alt->buffer[i], 0, (size_t)STEMS_ECHO_FRAMES * sizeof(Short2));
    memset(alt->line, 0, sizeof(alt->line));
    alt->cursor = alt->window = alt->running = 0u;
}

/* Under the lock: the package files next to pending_path, as far as they exist. */
static void stems_open_pending(struct stems_deck_context *context)
{
    static const char *suffix[3] = {".rx3stem", ".rx3drums", ".rx3bass"};
    char path[1024];
    size_t prefix = str_length(context->pending_path) - 8u;
    memcpy(path, context->pending_path, prefix);
    for (unsigned int i = 0; i < 3u; i++) {
        size_t size = str_length(suffix[i]) + 1u;
        if (prefix + size > sizeof(path)) break;
        memcpy(path + prefix, suffix[i], size);
        context->pending_fds[i] = open(path, O_RDONLY);
    }
}

static void stems_arm(unsigned int deck, struct stems_deck_context *context, void *reader);

static void stems_feature_track_will_load(unsigned int deck, void *reader,
                                          const void *track_info)
{
    (void)reader;
    if (deck >= 2u) return;
    struct stems_deck_context *context = &stems_decks[deck];
    stems_lock();
    __atomic_add_fetch(&context->generation, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&context->reader, 0, __ATOMIC_SEQ_CST);
    for (unsigned int i = 0; i < 3u; i++) {
        if (context->pending_fds[i] >= 0) close(context->pending_fds[i]);
        context->pending_fds[i] = -1;
    }
    context->pending_path[0] = 0;
    context->track_path[0] = 0;
    if (track_info) {
        const char *track = (const char *)track_info;
        size_t n = 0;
        while (track[n] && n + 1u < sizeof(context->track_path)) {
            context->track_path[n] = track[n];
            n++;
        }
        context->track_path[track[n] ? 0 : n] = 0;
    }
    if (!stem_path_for_track(track_info, context->pending_path, sizeof(context->pending_path)))
        stems_open_pending(context);
    context->pending_has_stem = context->pending_fds[0] >= 0;
    stems_unlock();
}


static void stems_feature_track_did_load(unsigned int deck, void *reader,
                                         const void *track_info)
{
    (void)track_info;
    if (deck >= 2u) return;
    struct stems_deck_context *context = &stems_decks[deck];
    stems_lock();
    __atomic_store_n(&context->reader, 0, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&context->generation, 1u, __ATOMIC_SEQ_CST);
    stems_drain(context);
    for (unsigned int i = 0; i < 3u; i++) release_payload(&context->payloads[i]);
    context->payload_count = 0u;
    stems_reset_mix(context);
    stems_echo_flush(deck);
    captured_pad_mask[deck] = 0u;
#ifdef RX3_OVERCUE_PROTOTYPE
    if(getenv("RX3_OVERCUE_ROOT")) {
        for(unsigned int i=0;i<3u;i++) {
            if(context->pending_fds[i]>=0)close(context->pending_fds[i]);
            context->pending_fds[i]=-1;
        }
        context->overcue=1u;
        rx3_overcue_track(deck,(const char *)track_info);
        __atomic_store_n(&context->selection,0x77u,__ATOMIC_SEQ_CST);
        stems_set_mask(context,7u);
        __atomic_store_n(&context->armed,1u,__ATOMIC_SEQ_CST);
        __atomic_store_n(&context->status,1u,__ATOMIC_SEQ_CST);
        __atomic_store_n(&context->reader,reader,__ATOMIC_SEQ_CST);
        stems_unlock();return;
    }
#endif
    stems_arm(deck, context, reader);
}

/* Under the lock, which it releases: publish what pending_fds hold and hand
   the read to the loader. The track's load, or MAKE STEMS arriving later. */
static void stems_arm(unsigned int deck, struct stems_deck_context *context, void *reader)
{
    stems_armed_ms[deck & 1u] = now_ms();
    unsigned int count = 0u;
    while (count < 3u && context->pending_fds[count] >= 0) count++;
    unsigned int available = count ? (2u << count) - 1u : 0u;
    unsigned int old = __atomic_load_n(&context->selection, __ATOMIC_SEQ_CST);
    __atomic_store_n(&context->selection, ((old & ~255u) + 256u) | available * 17u, __ATOMIC_SEQ_CST);
    stems_set_mask(context, available);
    __atomic_store_n(&context->armed, count != 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&context->status, count ? 1u : 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&context->reader, reader, __ATOMIC_SEQ_CST);
    struct stems_load_request *request = count ? mmap(0, 4096u, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
    if (request != MAP_FAILED) {
        request->context = context;
        request->reader = reader;
        request->generation = __atomic_load_n(&context->generation, __ATOMIC_SEQ_CST);
        for (unsigned int i = 0; i < 3u; i++) {
            request->fds[i] = context->pending_fds[i];
            context->pending_fds[i] = -1;
        }
        stems_release_request(stems_pending_loads[deck]);
        stems_pending_loads[deck] = request;
    } else {
        for (unsigned int i = 0; i < 3u; i++) {
            if (context->pending_fds[i] >= 0) close(context->pending_fds[i]);
            context->pending_fds[i] = -1;
        }
        if (count) {
            context->armed = 0;
            context->status = 3u;
        }
    }
    stems_unlock();
    if (request != MAP_FAILED) {
        const struct rx3_load_job job = {stems_load_pending, 0, 0};
        if (!framework->loader->submit(&stems_decks, &job))
            log_line("stems: loader queue full, the load waits for the next job");
    }
}

#endif /* RX3_STEMS_LOADER_H */
