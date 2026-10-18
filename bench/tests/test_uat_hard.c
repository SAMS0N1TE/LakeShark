/* CPFSK -> sampled phase discriminator -> packed hard samples; no RF. */
#include "ls_test.h"
#include "uat_hard.h"
#include "uat_stream.h"
#include <math.h>
#include <string.h>
static uint32_t rng = 0x978;
static unsigned random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static unsigned bits[1600], count;
static uint8_t hard[4096];
static size_t hard_n;
static void append(unsigned b) { bits[count++] = b; }
static void frame(bool lng, bool up, unsigned sync_errors)
{
    count = 0;
    for (int i = 0; i < 100; ++i) append(i & 1);
    uint64_t sync = up ? UAT_UPLINK_SYNC : UAT_DOWNLINK_SYNC;
    for (int i = 0; i < (int)sync_errors; ++i) sync ^= UINT64_C(1) << (i * 9);
    for (int i = 35; i >= 0; --i) append((sync >> i) & 1);
    uint8_t d[34] = {0}, f[48];
    d[0] = lng ? 8 : 0; d[1] = 0xab; d[2] = 0xcd; d[3] = 0xef;
    uint32_t lat = (uint32_t)(43.44 * 16777216.0 / 360);
    uint32_t lon = (uint32_t)((360 - 71.65) * 16777216.0 / 360);
    d[4] = lat >> 15; d[5] = lat >> 7; d[6] = (lat << 1) | (lon >> 23);
    d[7] = lon >> 15; d[8] = lon >> 7; d[9] = lon << 1; d[11] = 8;
    LS_CHECK(uat_rs_encode(d, lng ? 34 : 18, f));
    for (unsigned i = 0; i < (lng ? 384u : 240u); ++i) append((f[i/8] >> (7-i%8)) & 1);
    for (int i = 0; i < 160; ++i) append(0);
}
static void cpfsk(double rate, double ppm, bool noise)
{
    double symbol_rate = 1041666.666666667 * (1 + ppm / 1e6);
    double t = (random32() % 10000) / 10000.0 / symbol_rate;
    double carrier = (random32() % 10000) / 10000.0 * 6.283185307179586;
    memset(hard, 0, sizeof(hard));
    unsigned n = 0;
    while ((t + 1/rate) * symbol_rate < count) {
        double end = t + 1/rate, pos = t, angle = carrier;
        /* Exact CPFSK phase integration even when the interval straddles a
           symbol transition. Hard discriminator is sign(Im(conj(x0)*x1)). */
        while (pos < end - 1e-16) {
            unsigned k = (unsigned)(pos * symbol_rate + 1e-9);
            double stop = fmin(end, (k + 1) / symbol_rate);
            angle += 6.283185307179586 * (bits[k] ? 312500 : -312500) * (stop-pos);
            pos = stop;
        }
        unsigned b = (cos(carrier)*sin(angle) - sin(carrier)*cos(angle)) > 0;
        double middle = (t + end) / 2 * symbol_rate;
        unsigned k = (unsigned)middle;
        double frac = middle-k;
        bool near = (frac < .20 && k && bits[k] != bits[k-1]) ||
                    (frac > .80 && k+1 < count && bits[k] != bits[k+1]);
        if (noise && near && random32()%100 < 5) b ^= 1;
        hard[n/8] |= b << (7-n%8); ++n;
        carrier = remainder(angle, 6.283185307179586); t = end;
    }
    hard_n = n/8;
}
LS_CASE(sampled_cpfsk_basic_long_rates_clock_phase_and_transition_noise)
{
    const unsigned rates[] = {2000000,1500000,1250000};
    for (unsigned r = 0; r < 3; ++r) for (unsigned lng = 0; lng < 2; ++lng)
    for (int ppm = -200; ppm <= 200; ppm += 400) for (unsigned trial = 0; trial < 24; ++trial) {
        uat_hard_t s; uat_hard_init(&s,rates[r]);
        frame(lng,false,0); cpfsk(rates[r],ppm,true);
        for (size_t off = 0; off < hard_n;) {
            size_t n = 1 + random32()%31; if (n > hard_n-off) n = hard_n-off;
            uat_hard_feed(&s,hard+off,n,false); off += n;
        }
        LS_CHECK_MSG(s.stats.valid[lng] == 1,"rate=%u long=%u ppm=%d trial=%u sync=%u bad=%u",rates[r],lng,ppm,trial,s.stats.sync[0],s.stats.bad);
        LS_EQ_UINT(s.stats.sync[0],1);
        LS_EQ_UINT(s.stats.fix.address,0xabcdef); LS_CHECK(s.stats.fix.position);
        LS_NEAR(s.stats.fix.lat,43.44,.00003); LS_NEAR(s.stats.fix.lon,-71.65,.00003);
    }
}
LS_CASE(four_sync_errors_and_uplink_prefix)
{
    for (unsigned up = 0; up < 2; ++up) {
        uat_hard_t s; uat_hard_init(&s,2000000);
        frame(true,up,4); cpfsk(2000000,200,false);
        uat_hard_feed(&s,hard,hard_n,false);
        LS_EQ_UINT(s.stats.sync[up],1);
        LS_EQ_UINT(s.stats.valid[1],!up);
    }
}
LS_CASE(chip_one_megabit_resampling_bug_does_not_pass_rs)
{
    for (unsigned lng = 0; lng < 2; ++lng) for (unsigned trial = 0; trial < 24; ++trial) {
        uat_hard_t s; uat_hard_init(&s,1041667);
        frame(lng,false,0); cpfsk(1000000,0,false);
        uat_hard_feed(&s,hard,hard_n,false);
        LS_EQ_UINT(s.stats.valid[0]+s.stats.valid[1],0);
    }
}
LS_CASE(loss_seam_discards_partial_frame_then_reacquires)
{
    uat_hard_t s; uat_hard_init(&s,2000000);
    frame(true,false,0); cpfsk(2000000,-200,false);
    size_t split = hard_n/2;
    uat_hard_feed(&s,hard,split,false);
    uat_hard_feed(&s,hard+split+3,hard_n-split-3,true);
    LS_EQ_UINT(s.stats.valid[1],0); LS_EQ_UINT(s.stats.seams,1);
    uat_hard_feed(&s,hard,hard_n,true);
    LS_EQ_UINT(s.stats.valid[1],1); LS_EQ_UINT(s.stats.seams,2);
    LS_CHECK(s.stats.sync[0] >= 2);
}
LS_CASE(ring_wrap_overflow_and_seam_order)
{
    static uat_hard_ring_t ring;
    uat_hard_chunk_t out; uint8_t bytes[256];
    uat_hard_ring_init(&ring);
    /* Also cross the unsigned monotonic sequence number wrap. */
    atomic_store(&ring.write,UINT32_MAX-16); atomic_store(&ring.read,UINT32_MAX-16);
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < UAT_HARD_SLOTS; ++i) {
            memset(bytes,i,sizeof(bytes)); LS_CHECK(uat_hard_ring_push(&ring,bytes,sizeof(bytes),i==17));
        }
        LS_CHECK(!uat_hard_ring_push(&ring,bytes,1,false));
        for (unsigned i = 0; i < UAT_HARD_SLOTS; ++i) {
            LS_CHECK(uat_hard_ring_pop(&ring,&out)); LS_EQ_UINT(out.n,256);
            LS_EQ_UINT(out.bytes[0],i); LS_EQ_UINT(out.bytes[255],i);
            LS_EQ_UINT(out.seam,i==17 || (round && i==0));
        }
        LS_CHECK(!uat_hard_ring_pop(&ring,&out));
    }
    LS_CHECK(uat_hard_ring_push(&ring,bytes,1,false));
    LS_CHECK(uat_hard_ring_pop(&ring,&out)); LS_CHECK(out.seam);
    LS_CHECK(!uat_hard_ring_push(&ring,bytes,257,false));
    LS_CHECK(uat_hard_ring_push(&ring,bytes,256,false));
    LS_CHECK(uat_hard_ring_pop(&ring,&out)); LS_CHECK(out.seam);
}
LS_CASE(sync_acquires_without_an_artificial_training_preamble)
{
    const unsigned rates[]={2000000,1500000,1250000};
    for(unsigned r=0;r<3;++r) for(unsigned lng=0;lng<2;++lng) for(unsigned trial=0;trial<24;++trial) {
        uat_hard_t s;uat_hard_init(&s,rates[r]);frame(lng,false,0);
        memmove(bits,bits+92,(count-92)*sizeof(bits[0]));count-=92;
        for(unsigned i=0;i<8;++i) bits[i]=0;
        cpfsk(rates[r],trial&1 ? 200 : -200,true);
        /* Eight constant guard symbols model silence before the burst;
           there is no alternating training preamble. */
        uat_hard_feed(&s,hard,hard_n,false);
        LS_CHECK_MSG(s.stats.valid[lng]==1,"untrained rate=%u long=%u trial=%u",rates[r],lng,trial);
        LS_EQ_UINT(s.stats.fix.address,0xabcdef);
    }
}
LS_CASE(reader_chunks_through_ring_deliver_address_and_position_at_all_rates)
{
    const unsigned rates[]={2000000,1500000,1250000};
    static uat_hard_ring_t ring;
    for(unsigned r=0;r<3;++r) for(unsigned lng=0;lng<2;++lng) {
        uat_hard_t s;uat_hard_init(&s,rates[r]);uat_hard_ring_init(&ring);
        frame(lng,false,0);cpfsk(rates[r],r&1 ? 200 : -200,true);
        for(size_t offset=0;offset<hard_n;) {
            size_t n=1+random32()%67;if(n>hard_n-offset)n=hard_n-offset;
            LS_CHECK(uat_hard_ring_push(&ring,hard+offset,n,offset==0));offset+=n;
        }
        uat_hard_chunk_t c;
        while(uat_hard_ring_pop(&ring,&c))uat_hard_feed(&s,c.bytes,c.n,c.seam);
        LS_EQ_UINT(s.stats.valid[lng],1);LS_EQ_UINT(s.stats.sync[0],1);
        LS_EQ_UINT(s.stats.seams,1);LS_EQ_UINT(s.stats.fix.address,0xabcdef);
        LS_CHECK(s.stats.fix.position);LS_NEAR(s.stats.fix.lat,43.44,.00003);
        LS_NEAR(s.stats.fix.lon,-71.65,.00003);
    }
}
