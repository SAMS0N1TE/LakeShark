/* A bounded tap of the same checked IQ16 blocks sent to the live decoder. */
/* Development only: define LS_P25_LR_IQ_RECORD to build it. The hex dump is
   too heavy for the console while receiving, so releases leave it out. */
#ifdef LS_P25_LR_IQ_RECORD
#define LR_REC_BYTES 500000u
static uint8_t *s_lr_rec;
static atomic_uint s_lr_rec_state; /* 0 idle, 1 armed, 2 post-trigger, 3 ready, 4 gap */
static size_t s_lr_rec_pos, s_lr_rec_fill, s_lr_rec_post;
static uint64_t s_lr_rec_end;
static unsigned s_lr_rec_voice0, s_lr_rec_trigger, s_lr_rec_voice1;
static uint32_t s_lr_rec_hz;

static void p25_lr_record_feed(const lr2021_iq_block *b)
{
    unsigned state = atomic_load_explicit(&s_lr_rec_state, memory_order_acquire);
    if (state != 1 && state != 2) return;
    size_t first = LR_REC_BYTES - s_lr_rec_pos;
    if (first > b->bytes) first = b->bytes;
    memcpy(s_lr_rec + s_lr_rec_pos, b->data, first);
    memcpy(s_lr_rec, b->data + first, b->bytes - first);
    s_lr_rec_pos = (s_lr_rec_pos + b->bytes) % LR_REC_BYTES;
    s_lr_rec_fill += b->bytes;
    if (s_lr_rec_fill > LR_REC_BYTES) s_lr_rec_fill = LR_REC_BYTES;
    s_lr_rec_end = b->first_pair + b->bytes / 4;
    if (s_lr_rec_fill < LR_REC_BYTES) s_lr_rec_voice0 = P25.dsd_voice_count;
    if (state == 1 && s_lr_rec_fill == LR_REC_BYTES &&
        (unsigned)P25.dsd_voice_count > s_lr_rec_voice0) {
        s_lr_rec_trigger = P25.dsd_voice_count;
        s_lr_rec_post = 0;
        atomic_store_explicit(&s_lr_rec_state, 2, memory_order_release);
    } else if (state == 2) {
        s_lr_rec_post += b->bytes;
        if (s_lr_rec_post >= 100000) {
            s_lr_rec_voice1 = P25.dsd_voice_count;
            atomic_store_explicit(&s_lr_rec_state, 3, memory_order_release);
        }
    }
}
static void p25_lr_record_gap(void)
{
    unsigned state = atomic_load(&s_lr_rec_state);
    if (state == 1 || state == 2) atomic_store(&s_lr_rec_state, 4);
}
int p25_lr_iq_record_command(int argc, char **argv)
{
    unsigned state = atomic_load_explicit(&s_lr_rec_state, memory_order_acquire);
    if (argc == 1 && !strcmp(argv[0], "arm")) {
        if (!P25.lr_iq_active || state == 1 || state == 2) return 1;
        if (!s_lr_rec) s_lr_rec = heap_caps_malloc(LR_REC_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_lr_rec) return 1;
        s_lr_rec_pos = s_lr_rec_fill = s_lr_rec_post = 0;
        s_lr_rec_voice0 = P25.dsd_voice_count;
        s_lr_rec_hz = s_radio_freq_hz;
        atomic_store_explicit(&s_lr_rec_state, 1, memory_order_release);
        puts("LRIQREC armed; waiting for decoded voice");
        return 0;
    }
    if (argc == 1 && !strcmp(argv[0], "dump")) {
        if (state != 3) { puts("LRIQREC not ready"); return 1; }
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t off = 0; off < LR_REC_BYTES; off += 64) {
            char line[180];
            int n = snprintf(line, sizeof(line), "LRIQDATA %lu ", (unsigned long)off);
            size_t count = LR_REC_BYTES - off; if (count > 64) count = 64;
            for (size_t i = 0; i < count; i++) {
                uint8_t v = s_lr_rec[(s_lr_rec_pos + off + i) % LR_REC_BYTES];
                n += snprintf(line + n, sizeof(line) - n, "%02X", v);
                crc ^= v;
                for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xEDB88320u);
            }
            line[n++] = '\n'; line[n] = 0;
            fputs(line, stdout);
            if (!(off % 4096)) vTaskDelay(1);
        }
        printf("LRIQREC bytes=%u crc=%08lX rate=125000 hz=%lu first=%llu trigger_voice=%u end_voice=%u\n",
            LR_REC_BYTES, (unsigned long)~crc, (unsigned long)s_lr_rec_hz,
            (unsigned long long)(s_lr_rec_end - LR_REC_BYTES / 4), s_lr_rec_trigger, s_lr_rec_voice1);
        return 0;
    }
    printf("LRIQREC state=%u (1=armed 2=triggered 3=ready 4=gap)\n", state);
    return 0;
}
#else
static inline void p25_lr_record_feed(const lr2021_iq_block *b) { (void)b; }
static inline void p25_lr_record_gap(void) {}
int p25_lr_iq_record_command(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("LRIQREC not in this build");
    return 1;
}
#endif
