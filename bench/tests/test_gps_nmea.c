/* LS_TEST_SOURCES: ${FW}/components/lakeshark/board/ls_gps.c */
/* The NMEA parser, against sentences that carry a position. */

#include "ls_test.h"
#include "ls_gps.h"

#include <string.h>

/* The reader half is not under test: it is a UART, a task and a ring buffer,
   and the parser was separated from it precisely so this file does not need
   any of that. These exist so the object links. */
#include "driver/uart.h"

bool uart_is_driver_installed(int p) { (void)p; return false; }
esp_err_t uart_driver_install(int p, int rx, int tx, int q,
                              void *qh, int flags)
{ (void)p; (void)rx; (void)tx; (void)q; (void)qh; (void)flags; return ESP_OK; }
esp_err_t uart_driver_delete(int p) { (void)p; return ESP_OK; }
esp_err_t uart_set_pin(int p, int tx, int rx, int rts, int cts)
{ (void)p; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }
int uart_read_bytes(int p, void *buf, uint32_t len, uint32_t ticks)
{ (void)p; (void)buf; (void)len; (void)ticks; return 0; }
esp_err_t uart_param_config(int p, const uart_config_t *c)
{ (void)p; (void)c; return ESP_OK; }
int uart_write_bytes(int p, const void *src, size_t len)
{ (void)p; (void)src; return (int)len; }
esp_err_t uart_set_baudrate(int p, uint32_t baud)
{ (void)p; (void)baud; return ESP_OK; }
esp_err_t uart_wait_tx_done(int p, uint32_t ticks)
{ (void)p; (void)ticks; return ESP_OK; }
esp_err_t uart_flush_input(int p) { (void)p; return ESP_OK; }

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = NULL;
    return pdPASS;
}

/* Sentence, without the terminator. */
static bool feed(ls_gps_parser_t *p, const char *line)
{
    return ls_gps_parse_line(line, (int)strlen(line), p);
}

static void fresh(ls_gps_parser_t *p) { memset(p, 0, sizeof(*p)); }

/* Builds "$<payload>*HH" with the checksum computed from the payload, so a
   sentence assembled for a test is never at odds with what it claims to
   carry. */
static void checksum_line(char *out, size_t outsz, const char *payload)
{
    uint8_t sum = 0;
    for (const char *p = payload; *p; p++) sum ^= (uint8_t)*p;
    snprintf(out, outsz, "$%s*%02X", payload, sum);
}

/* A GSA carrying the given PRN strings (each e.g. "04", or "" to leave a slot
   empty) in its twelve satellite-ID fields. */
static void gsa_line(char *out, size_t outsz, const char *const *prns, int nprns)
{
    char payload[96];
    int pos = snprintf(payload, sizeof(payload), "GPGSA,A,3");
    for (int i = 0; i < 12; i++) {
        const char *v = (i < nprns) ? prns[i] : "";
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",%s", v);
    }
    snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",2.5,1.3,2.1");
    checksum_line(out, outsz, payload);
}

/* A one-message GSV series reporting a single satellite. */
static void gsv_line(char *out, size_t outsz, const char *prn,
                     int elevation, int azimuth, int snr)
{
    char payload[96];
    snprintf(payload, sizeof(payload), "GPGSV,1,1,01,%s,%02d,%03d,%02d",
             prn, elevation, azimuth, snr);
    checksum_line(out, outsz, payload);
}

/* A GSA from the given talker, with NMEA 4.1's system ID in its last field
   when sysid is not zero. */
static void gsa_line_sys(char *out, size_t outsz, const char *talker,
                         const char *const *prns, int nprns, int sysid)
{
    char payload[96];
    int pos = snprintf(payload, sizeof(payload), "%sGSA,A,3", talker);
    for (int i = 0; i < 12; i++) {
        const char *v = (i < nprns) ? prns[i] : "";
        pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",%s", v);
    }
    pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",2.5,1.3,2.1");
    if (sysid)
        snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",%d", sysid);
    checksum_line(out, outsz, payload);
}

/* One talker's whole GSV series: n satellites numbered from first_prn, each
   at the given C/N0, or with the field left empty when snr is 0. */
static void feed_gsv_series(ls_gps_parser_t *p, const char *talker, int n,
                            int first_prn, int snr)
{
    const int msgs = (n + 3) / 4;
    for (int m = 0; m < msgs; m++) {
        char payload[120], line[130];
        int pos = snprintf(payload, sizeof(payload), "%sGSV,%d,%d,%02d",
                           talker, msgs, m + 1, n);
        for (int k = m * 4; k < n && k < m * 4 + 4; k++) {
            pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                            ",%02d,%02d,%03d,", first_prn + k, 10 + k,
                            (k * 37) % 360);
            if (snr)
                pos += snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                                "%02d", snr);
        }
        checksum_line(line, sizeof(line), payload);
        LS_CHECK_MSG(feed(p, line), "rejected %s", line);
    }
}

static const char GGA_FIX[] =
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
static const char GGA_NONE[] = "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E";

LS_CASE(position_freshness_is_not_refreshed_by_satellites_or_void_rmc)
{
    ls_gps_parser_t p = {0};
    LS_CHECK(feed(&p,"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_EQ_INT(p.st.position_updates,1);
    const char *payload="GPRMC,123520,V,,,,,,,230394,,,N";
    unsigned checksum=0;
    for(const char *c=payload;*c;++c) checksum^=(unsigned char)*c;
    char line[90]; snprintf(line,sizeof(line),"$%s*%02X",payload,checksum);
    LS_CHECK(feed(&p,line)); LS_CHECK(!p.st.fix); LS_EQ_INT(p.st.position_updates,1);
}

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_bad_checksum_changes_nothing)
{
    /* The first line of defence. A sentence damaged in transit must not move
       a position, and at 115200 on a shared board damage happens. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    double lat = p.st.lat_deg, lon = p.st.lon_deg;

    /* Same sentence with one digit changed and a checksum that does not
       match it. The value is deliberately wrong: a helper that recomputes
       checksums across this file must leave this one alone. */
    LS_CHECK_MSG(!feed(&p, "$GPGGA,123519,4907.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*B9"),
                 "a sentence with a broken checksum was accepted");
    LS_CHECK_MSG(p.st.lat_deg == lat && p.st.lon_deg == lon,
                 "a rejected sentence still moved the position");
}

LS_CASE(degrees_and_minutes_become_decimal_degrees)
{
    /* The conversion that has never run. 4807.038 N is 48 degrees and 7.038
       minutes: 48 + 7.038/60 = 48.1173. Getting this wrong by leaving it as
       4807.038, or as 48.07038, puts the device somewhere impossible or a
       few kilometres out, and the second is the one nobody notices. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));

    LS_CHECK(p.st.fix);
    LS_NEAR(48.1173, p.st.lat_deg, 0.0001);
    LS_NEAR(11.5167, p.st.lon_deg, 0.0001);
}

LS_CASE(longitude_has_three_degree_digits_and_latitude_two)
{

    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_CHECK_MSG(p.st.lon_deg > 11.0 && p.st.lon_deg < 12.0,
                 "longitude parsed as %f, expected about 11.52", p.st.lon_deg);

    /* And a three-digit longitude past 100 degrees still works. */
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,001043.00,4404.14036,N,12118.85961,W,1,12,0.98,1113.0,M,-21.3,M,,*59"));
    LS_NEAR(44.0690, p.st.lat_deg, 0.001);
    LS_NEAR(-121.3143, p.st.lon_deg, 0.001);
}

LS_CASE(south_and_west_are_negative)
{
    /* Sydney: south and east. A missing sign puts it in Russia. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,062101,3352.000,S,15112.000,E,1,09,0.8,20.0,M,20.0,M,,*6B"));
    LS_CHECK_MSG(p.st.lat_deg < 0, "a southern latitude came out as %f", p.st.lat_deg);
    LS_CHECK_MSG(p.st.lon_deg > 0, "an eastern longitude came out as %f", p.st.lon_deg);
    LS_NEAR(-33.8667, p.st.lat_deg, 0.001);
    LS_NEAR(151.2000, p.st.lon_deg, 0.001);
}

LS_CASE(an_empty_position_is_not_a_fix_at_the_equator)
{
    /* What the board has actually been producing all along: a framed sentence
       with nothing in it. The dangerous failure is treating those empty
       fields as zero and reporting a confident fix in the Gulf of Guinea. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E"));

    LS_CHECK_MSG(!p.st.fix, "an empty GGA was reported as a fix");
    LS_EQ_INT(0, p.st.quality);
    LS_EQ_INT(0, p.st.sats_used);
}

LS_CASE(losing_a_fix_clears_it)
{
    /* A receiver that goes indoors keeps sending, with quality back to 0.
       Leaving `fix` true would show a stale position as current. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_CHECK(p.st.fix);

    LS_CHECK(feed(&p, "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E"));
    LS_CHECK_MSG(!p.st.fix, "the fix survived a quality of zero");
}

LS_CASE(rmc_carries_a_position_a_date_and_a_speed)
{
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"));

    LS_CHECK(p.st.fix);
    LS_NEAR(48.1173, p.st.lat_deg, 0.0001);
    LS_NEAR(22.4, p.st.speed_kts, 0.1);
    LS_NEAR(84.4, p.st.course_deg, 0.1);
    LS_EQ_INT(23, p.st.day);
    LS_EQ_INT(3, p.st.month);
    LS_EQ_INT(1994, p.st.year);
}

LS_CASE(an_rmc_marked_void_does_not_place_the_receiver)
{
    /* Status V means the data is not valid. Reading the coordinates anyway
       is how a receiver that is still searching reports a position. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*7D"));
    LS_CHECK_MSG(!p.st.fix, "a void RMC was treated as a fix");
}

LS_CASE(the_clock_comes_out_of_the_time_field)
{
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_EQ_INT(12, p.st.hour);
    LS_EQ_INT(35, p.st.minute);
    LS_EQ_INT(19, p.st.second);

    /* Fractional seconds are common and must not shift the fields. */
    fresh(&p);
    LS_CHECK(feed(&p, "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E"));
    LS_EQ_INT(4, p.st.hour);
    LS_EQ_INT(47, p.st.minute);
    LS_EQ_INT(50, p.st.second);
}

LS_CASE(satellites_in_view_are_summed_across_constellations_once_a_cycle)
{
    /* Each constellation sends its own GSV series and repeats its count in
       every message of that series, so only the first of each may be added.
       GGA closes the cycle and publishes. */
    ls_gps_parser_t p;
    fresh(&p);

    /* GPS: 11 in view, over three messages. */
    LS_CHECK(feed(&p, "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74"));
    LS_CHECK(feed(&p, "$GPGSV,3,2,11,14,25,170,00,16,57,208,39,18,67,296,40,19,40,246,00*74"));
    /* GLONASS: 4 more. */
    LS_CHECK(feed(&p, "$GLGSV,1,1,04,65,10,120,00,66,45,230,35,67,20,300,28,68,05,050,00*62"));

    /* Nothing published until a cycle closes. */
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_CHECK_MSG(p.st.sats_visible == 15,
                 "expected 11 + 4 satellites in view, got %d", p.st.sats_visible);

    /* A second series repeated in the same message must not double count. */
    LS_EQ_INT(0, p.sats_acc);
}

LS_CASE(the_talker_id_does_not_matter)
{
    /* GP, GN, GL, GA - the constellation prefix varies with what the receiver
       is tracking and the sentence means the same thing. */
    static const char *const SAME[] = {
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
        "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*59",
    };
    for (unsigned i = 0; i < sizeof(SAME) / sizeof(SAME[0]); i++) {
        ls_gps_parser_t p;
        fresh(&p);
        LS_CHECK_MSG(feed(&p, SAME[i]), "sentence %u was rejected", i);
        LS_CHECK(p.st.fix);
        LS_NEAR(48.1173, p.st.lat_deg, 0.0001);
    }
}

LS_CASE(a_sentence_the_parser_does_not_know_is_ignored_quietly)
{
    /* A receiver emits VTG, GLL, GSA and more. They are not errors and must
       not disturb what the known ones established. */
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(feed(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    double lat = p.st.lat_deg;

    LS_CHECK(feed(&p, "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48"));
    LS_CHECK(feed(&p, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39"));
    LS_CHECK_MSG(p.st.lat_deg == lat, "an unknown sentence moved the position");
    LS_CHECK(p.st.fix);
}

LS_CASE(rubbish_is_refused_rather_than_parsed)
{
    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(!feed(&p, ""));
    LS_CHECK(!feed(&p, "$"));
    LS_CHECK(!feed(&p, "$GPGGA"));
    LS_CHECK(!feed(&p, "not a sentence at all"));
    LS_CHECK(!feed(&p, "$GPGGA,123519,4807.038,N*"));
    LS_CHECK(!feed(&p, "$GPGGA,123519,4807.038,N*ZZ"));
    LS_CHECK(!ls_gps_parse_line(NULL, 10, &p));
    LS_CHECK(!ls_gps_parse_line("$GPGGA*56", 9, NULL));
    LS_CHECK_MSG(!p.st.fix, "rubbish produced a fix");
}

LS_CASE(a_sentence_longer_than_nmea_allows_is_refused)
{
    /* 82 bytes is the limit. Anything longer is a framing fault, and a parser
       that copies it into a fixed buffer without checking is the classic
       overflow. */
    ls_gps_parser_t p;
    fresh(&p);
    char big[200];
    memset(big, 'A', sizeof(big));
    big[0] = '$';
    memcpy(big + sizeof(big) - 3, "*00", 3);
    LS_CHECK(!ls_gps_parse_line(big, (int)sizeof(big), &p));
}

LS_CASE(the_input_sentence_is_not_modified)
{
    /* The reader hands the same buffer to the parser and then reuses it. A
       parser that writes terminators into its input would corrupt the next
       sentence assembled there. */
    char line[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    char copy[sizeof(line)];
    memcpy(copy, line, sizeof(line));

    ls_gps_parser_t p;
    fresh(&p);
    LS_CHECK(ls_gps_parse_line(line, (int)strlen(line), &p));
    LS_CHECK_MSG(memcmp(line, copy, sizeof(line)) == 0,
                 "the parser modified the sentence it was given");
}

/* ------------------------------------------------- GSA/GSV ordering cases -- */

LS_CASE(gsa_before_gsv_still_marks_the_satellite_used)
{
    /* GSA and GSV are not ordered relative to each other by NMEA. A GSA
       naming PRN 04 before the GSV that first reports it must not lose the
       used flag once that satellite's entry exists. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    const char *prns[] = { "04" };
    gsa_line(line, sizeof(line), prns, 1);
    LS_CHECK(feed(&p, line));

    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    LS_CHECK(p.st.fix);
    LS_EQ_INT(1, p.st.quality);
    LS_EQ_INT(8, p.st.sats_used);
    LS_CHECK_MSG(p.st.sat_count >= 1, "expected at least one published satellite");
    LS_CHECK_MSG(p.st.sats[0].prn == 4, "expected PRN 4, got %d", p.st.sats[0].prn);
    LS_CHECK_MSG(p.st.sats[0].used, "GSA-before-GSV lost the used flag for PRN 4");
}

LS_CASE(gsv_before_gsa_also_marks_the_satellite_used)
{
    /* The order the driver has been fed most often. Must keep working. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));

    const char *prns[] = { "04" };
    gsa_line(line, sizeof(line), prns, 1);
    LS_CHECK(feed(&p, line));

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    LS_CHECK_MSG(p.st.sats[0].used,
                 "PRN 4 should be used regardless of sentence order");
}

LS_CASE(a_remembered_gsa_prn_does_not_leak_into_the_next_cycle)
{
    /* A PRN a GSA marked used belongs to the cycle it arrived in. A GSV that
       reports the same PRN next cycle, with no GSA of its own, must not
       inherit the previous cycle's flag. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    const char *prns[] = { "04" };
    gsa_line(line, sizeof(line), prns, 1);
    LS_CHECK(feed(&p, line));

    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));
    LS_CHECK(p.st.sats[0].used);

    /* Second cycle: same PRN reported by GSV, no GSA this time. */
    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    LS_CHECK_MSG(!p.st.sats[0].used,
                 "a GSA from a previous cycle still marked this cycle's satellite used");
}

LS_CASE(a_duplicate_gsa_prn_id_is_deduplicated)
{
    /* The same PRN can appear more than once across a GSA sentence's own
       fields, or across the sentences some receivers split it into. Neither
       should grow the remembered set past one entry per satellite or change
       the outcome. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    const char *prns[] = { "04", "04" };
    gsa_line(line, sizeof(line), prns, 2);
    LS_CHECK(feed(&p, line));
    /* A second GSA sentence repeating the same PRN. */
    LS_CHECK(feed(&p, line));

    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));

    checksum_line(line, sizeof(line),
                  "GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    LS_CHECK(feed(&p, line));

    LS_CHECK(p.st.sats[0].used);
    LS_EQ_INT(1, p.st.sat_count);
}

LS_CASE(no_false_fix_when_quality_is_zero_despite_satellites_used)
{
    /* A GSA/GSV pair naming a tracked satellite is not a fix. Quality 0 must
       stay a non-fix even though a used flag is published. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];

    const char *prns[] = { "04" };
    gsa_line(line, sizeof(line), prns, 1);
    LS_CHECK(feed(&p, line));

    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));

    checksum_line(line, sizeof(line),
                  "GNGGA,044750.600,,,,,0,00,25.5,,,,,");
    LS_CHECK(feed(&p, line));

    LS_CHECK_MSG(!p.st.fix,
                 "quality 0 must not be reported as a fix even with tracked satellites");
    LS_EQ_INT(0, p.st.quality);
    LS_CHECK_MSG(p.st.sats[0].used,
                 "GSA-reported used flag should still publish even without a fix");
}

LS_CASE(out_of_range_gsa_prns_do_not_alias_valid_satellites)
{
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];
    const char *prns[] = { "260", "-252", "0" };
    gsa_line(line, sizeof(line), prns, 3);
    LS_CHECK(feed(&p, line));
    gsv_line(line, sizeof(line), "04", 15, 270, 40);
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "GNGGA,044750.600,,,,,0,00,25.5,,,,,");
    LS_CHECK(feed(&p, line));
    LS_EQ_INT(1, p.st.sat_count);
    LS_CHECK(!p.st.sats[0].used);
    LS_CHECK(!p.st.fix);
}

/* ------------------------------------------------ constellations and size -- */

LS_CASE(gps_prn_5_and_beidou_prn_5_are_different_satellites)
{
    /* NMEA 4.1 numbers each constellation from 1, so GPS 5 and BeiDou 5 both
       arrive as "05". The GSA that puts one of them in the fix names its
       system in the last field; keyed on the PRN alone, both would be used. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];
    const char *prns[] = { "05" };

    checksum_line(line, sizeof(line), "GPGSV,1,1,01,05,40,083,30,1");
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "GBGSV,1,1,01,05,55,200,35,1");
    LS_CHECK(feed(&p, line));
    gsa_line_sys(line, sizeof(line), "GN", NULL, 0, 1);      /* GPS: none */
    LS_CHECK(feed(&p, line));
    gsa_line_sys(line, sizeof(line), "GN", prns, 1, 4);      /* BeiDou: 05 */
    LS_CHECK(feed(&p, line));
    LS_CHECK(feed(&p, GGA_FIX));

    LS_EQ_INT(2, p.st.sat_count);
    LS_CHECK_MSG(!p.st.sats[0].used, "GPS PRN 5 was marked used by BeiDou's GSA");
    LS_CHECK_MSG(p.st.sats[1].used, "BeiDou PRN 5 lost its used flag");
}

LS_CASE(the_same_holds_when_the_gsa_comes_first)
{
    /* The remembered set is keyed the same way, so a GSA ahead of its GSV
       cannot mark the other constellation's satellite either. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];
    const char *prns[] = { "05" };

    gsa_line_sys(line, sizeof(line), "GN", prns, 1, 4);
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "GPGSV,1,1,01,05,40,083,30,1");
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "GBGSV,1,1,01,05,55,200,35,1");
    LS_CHECK(feed(&p, line));
    LS_CHECK(feed(&p, GGA_FIX));

    LS_EQ_INT(2, p.st.sat_count);
    LS_CHECK_MSG(!p.st.sats[0].used, "GPS PRN 5 inherited BeiDou's used flag");
    LS_CHECK_MSG(p.st.sats[1].used, "BeiDou PRN 5 lost its used flag");
}

LS_CASE(the_table_holds_forty_four_satellites)
{
    /* GPS, GLONASS, BeiDou and Galileo together, as a clear sky gives them
       with three or four constellations on. */
    ls_gps_parser_t p;
    fresh(&p);
    feed_gsv_series(&p, "GP", 12, 1, 30);
    feed_gsv_series(&p, "GL", 8, 65, 28);
    feed_gsv_series(&p, "GB", 16, 1, 33);
    feed_gsv_series(&p, "GA", 8, 1, 25);
    LS_CHECK(feed(&p, GGA_FIX));

    LS_EQ_INT(44, p.st.sats_visible);
    LS_CHECK_MSG(p.st.sat_count == 44, "the table kept %d of 44", p.st.sat_count);
    LS_EQ_INT(8, p.st.sats[43].prn);
}

LS_CASE(the_snapshot_is_small)
{
    /* ls_gps_get copies one of these onto the stack of each task that asks.
       The parser's working tables live in ls_gps_parser_t, which only the
       reader owns, so a snapshot is the published table and the scalars
       around it and not the accumulators behind them. */
    ls_note("ls_gps_state_t is %u bytes, ls_gps_parser_t %u",
            (unsigned)sizeof(ls_gps_state_t), (unsigned)sizeof(ls_gps_parser_t));
    LS_CHECK_MSG(sizeof(ls_gps_state_t) <= 420,
                 "a snapshot is %u bytes, over the 420 it may take",
                 (unsigned)sizeof(ls_gps_state_t));
}

LS_CASE(a_gsa_without_a_system_id_is_keyed_by_its_talker)
{
    /* NMEA 4.0 receivers send a GSA per constellation under its own talker
       and no system ID. BD is BeiDou, as surely as the ID 4 would be. */
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];
    const char *prns[] = { "05" };

    checksum_line(line, sizeof(line), "GPGSV,1,1,01,05,40,083,30");
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "BDGSV,1,1,01,05,55,200,35");
    LS_CHECK(feed(&p, line));
    gsa_line_sys(line, sizeof(line), "BD", prns, 1, 0);
    LS_CHECK(feed(&p, line));
    LS_CHECK(feed(&p, GGA_FIX));

    LS_EQ_INT(LS_GPS_SYS_GPS, p.st.sats[0].sys);
    LS_EQ_INT(LS_GPS_SYS_BEIDOU, p.st.sats[1].sys);
    LS_CHECK_MSG(!p.st.sats[0].used, "the BeiDou GSA marked GPS PRN 5 used");
    LS_CHECK(p.st.sats[1].used);

    /* A GN GSA with no system ID cannot say which it means, so it keeps
       the PRN-only match: both. */
    fresh(&p);
    checksum_line(line, sizeof(line), "GPGSV,1,1,01,05,40,083,30");
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "BDGSV,1,1,01,05,55,200,35");
    LS_CHECK(feed(&p, line));
    gsa_line_sys(line, sizeof(line), "GN", prns, 1, 0);
    LS_CHECK(feed(&p, line));
    LS_CHECK(feed(&p, GGA_FIX));
    LS_CHECK(p.st.sats[0].used && p.st.sats[1].used);
}

LS_CASE(the_antenna_status_comes_from_the_txt_sentence)
{
    ls_gps_parser_t p;
    fresh(&p);
    LS_EQ_INT(LS_GPS_ANT_UNKNOWN, p.st.antenna);
    LS_EQ_STR("not reported", ls_gps_antenna_name(p.st.antenna));

    LS_CHECK(feed(&p, "$GPTXT,01,01,01,ANTENNA OPEN*25"));
    LS_EQ_INT(LS_GPS_ANT_OPEN, p.st.antenna);

    char line[100];
    checksum_line(line, sizeof(line), "GPTXT,01,01,01,ANTENNA OK");
    LS_CHECK(feed(&p, line));
    LS_EQ_INT(LS_GPS_ANT_OK, p.st.antenna);

    checksum_line(line, sizeof(line), "GNTXT,01,01,01,ANTENNA SHORT");
    LS_CHECK(feed(&p, line));
    LS_EQ_INT(LS_GPS_ANT_SHORT, p.st.antenna);
    LS_EQ_STR("SHORT", ls_gps_antenna_name(p.st.antenna));

    /* Other text, and a word this does not know, leave the last report. */
    checksum_line(line, sizeof(line), "GPTXT,01,01,02,MS=7,5,1");
    LS_CHECK(feed(&p, line));
    checksum_line(line, sizeof(line), "GPTXT,01,01,01,ANTENNA UNKNOWN");
    LS_CHECK(feed(&p, line));
    LS_EQ_INT(LS_GPS_ANT_SHORT, p.st.antenna);
}

LS_CASE(tracked_satellites_are_counted_per_constellation)
{
    /* NMEA 4.1 GSV with signal IDs, and the empty C/N0 an almanac-only
       satellite has. In view is 10; tracked is the 6 with a C/N0. */
    ls_gps_parser_t p;
    fresh(&p);
    static const char *const SKY[] = {
        "GPGSV,2,1,05,01,40,083,32,03,,,,07,22,300,28,08,05,180,,1",
        "GPGSV,2,2,05,11,60,045,41,1",
        "GLGSV,1,1,02,65,30,100,25,66,10,200,,1",
        "GBGSV,1,1,03,05,45,120,36,10,,,,13,70,010,30,1",
    };
    char line[100];
    for (unsigned i = 0; i < sizeof(SKY) / sizeof(SKY[0]); i++) {
        checksum_line(line, sizeof(line), SKY[i]);
        LS_CHECK_MSG(feed(&p, line), "rejected %s", line);
    }
    LS_EQ_INT(0, p.st.sats_tracked);              /* not until GGA publishes */
    LS_CHECK(feed(&p, GGA_FIX));

    LS_EQ_INT(10, p.st.sats_visible);
    LS_CHECK_MSG(p.st.sat_count == 10,
                 "%d entries: the signal ID became a satellite", p.st.sat_count);
    LS_EQ_INT(6, p.st.sats_tracked);
    LS_EQ_INT(3, p.st.tracked_by_sys[LS_GPS_SYS_GPS]);
    LS_EQ_INT(1, p.st.tracked_by_sys[LS_GPS_SYS_GLONASS]);
    LS_EQ_INT(2, p.st.tracked_by_sys[LS_GPS_SYS_BEIDOU]);
    LS_EQ_INT(0, p.st.tracked_by_sys[LS_GPS_SYS_GALILEO]);
    LS_EQ_INT(41, p.st.cn0_best);
    /* 41, 36, 32 and 30: 34.75, rounded. */
    LS_EQ_INT(35, p.st.cn0_top4);

    /* PRN 03 has no position and no C/N0; PRN 65 is GLONASS. */
    LS_EQ_INT(3, p.st.sats[1].prn);
    LS_EQ_INT(0, p.st.sats[1].snr);
    LS_EQ_INT(LS_GPS_SYS_GPS, p.st.sats[1].sys);
    LS_EQ_INT(65, p.st.sats[5].prn);
    LS_EQ_INT(LS_GPS_SYS_GLONASS, p.st.sats[5].sys);
    LS_EQ_INT(LS_GPS_SYS_BEIDOU, p.st.sats[9].sys);

    char text[200];
    ls_gps_describe_sky(text, sizeof(text), &p.st, 0);
    LS_EQ_STR("tracked 6 (GPS 3, BeiDou 2, GLONASS 1), C/N0 best 41, "
              "top-4 mean 35 dB-Hz, antenna not reported, no fix", text);
}

LS_CASE(fewer_than_four_tracked_average_what_there_is)
{
    ls_gps_parser_t p;
    fresh(&p);
    char line[100];
    checksum_line(line, sizeof(line),
                  "GPGSV,1,1,03,01,40,083,30,02,20,100,,04,50,200,25");
    LS_CHECK(feed(&p, line));
    LS_CHECK(feed(&p, GGA_FIX));
    LS_EQ_INT(2, p.st.sats_tracked);
    LS_EQ_INT(30, p.st.cn0_best);
    LS_EQ_INT(28, p.st.cn0_top4);                 /* 27.5, rounded */

    /* Nothing tracked says so, rather than a mean of nothing. */
    fresh(&p);
    feed_gsv_series(&p, "GP", 5, 1, 0);
    LS_CHECK(feed(&p, GGA_NONE));
    LS_EQ_INT(5, p.st.sat_count);
    LS_EQ_INT(0, p.st.sats_tracked);
    LS_EQ_INT(0, p.st.cn0_best);
    p.st.antenna = LS_GPS_ANT_OPEN;
    char text[200];
    ls_gps_describe_sky(text, sizeof(text), &p.st, 0);
    LS_EQ_STR("tracked 0, no C/N0, antenna OPEN, no fix", text);
}

LS_CASE(time_to_first_fix_counts_from_the_reader_start)
{
    ls_gps_parser_t p;
    fresh(&p);
    p.st.start_us = 1000000;                      /* the reader started at 1 s */

    LS_CHECK(ls_gps_feed_line(GGA_NONE, (int)strlen(GGA_NONE), &p, 2000000));
    LS_CHECK(p.st.alive);
    LS_CHECK(p.st.first_fix_us == 0);

    char text[200];
    ls_gps_describe_sky(text, sizeof(text), &p.st, 841000000);
    LS_CHECK_MSG(strstr(text, ", no fix after 840 s") != NULL, "%s", text);

    LS_CHECK(ls_gps_feed_line(GGA_FIX, (int)strlen(GGA_FIX), &p, 42000000));
    LS_CHECK(p.st.first_fix_us == 42000000);
    LS_CHECK(ls_gps_feed_line(GGA_FIX, (int)strlen(GGA_FIX), &p, 43000000));
    LS_CHECK_MSG(p.st.first_fix_us == 42000000, "the first fix moved");
    LS_CHECK(p.st.last_fix_us == 43000000);
    LS_CHECK(p.st.last_sentence_us == 43000000);
    LS_EQ_INT(3, p.st.sentences);

    ls_gps_describe_sky(text, sizeof(text), &p.st, 50000000);
    LS_CHECK_MSG(strstr(text, ", first fix after 41 s") != NULL, "%s", text);

    /* A damaged line is counted as one and changes nothing else. */
    LS_CHECK(!ls_gps_feed_line("$GPGGA,1*00", 11, &p, 44000000));
    LS_EQ_INT(1, p.st.checksum_errors);
    LS_EQ_INT(3, p.st.sentences);
    LS_CHECK(p.st.last_sentence_us == 43000000);
}

/* ------------------------------------------------------- module bring-up -- */

/* Steps the bring-up with nothing framing, up to `until` us. */
static ls_gps_do_t step_quiet(ls_gps_link_t *link, int64_t until,
                              bool configured)
{
    ls_gps_do_t act = LS_GPS_DO_NOTHING;
    for (int64_t t = 0; t <= until && act == LS_GPS_DO_NOTHING; t += 200000)
        act = ls_gps_link_step(link, false, t, configured);
    return act;
}

LS_CASE(a_module_at_115200_is_configured_once_and_left_alone)
{
    ls_gps_link_t link = LS_GPS_LINK_PROBE;
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, false, 300000, false));
    LS_EQ_INT(LS_GPS_DO_CONFIGURE, ls_gps_link_step(&link, true, 400000, false));
    LS_EQ_INT(LS_GPS_LINK_UP, link);
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, true, 0, true));
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, false, 60000000, true));
    LS_EQ_INT(LS_GPS_LINK_UP, link);

    /* A later start the same boot finds it configured. */
    link = LS_GPS_LINK_PROBE;
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, true, 100000, true));
    LS_EQ_INT(LS_GPS_LINK_UP, link);
}

LS_CASE(a_module_at_9600_is_moved_saved_then_configured)
{
    ls_gps_link_t link = LS_GPS_LINK_PROBE;
    LS_EQ_INT(LS_GPS_DO_NOTHING,
              ls_gps_link_step(&link, false, LS_GPS_PROBE_US - 1, false));
    LS_EQ_INT(LS_GPS_DO_LISTEN_9600,
              ls_gps_link_step(&link, false, LS_GPS_PROBE_US, false));
    LS_EQ_INT(LS_GPS_LINK_TRY_9600, link);

    LS_EQ_INT(LS_GPS_DO_RAISE, ls_gps_link_step(&link, true, 300000, false));
    LS_EQ_INT(LS_GPS_LINK_CONFIRM, link);
    /* Heard at 115200: the rate is saved before anything else goes out. */
    LS_EQ_INT(LS_GPS_DO_SAVE, ls_gps_link_step(&link, true, 500000, false));
    LS_EQ_INT(LS_GPS_LINK_UP, link);
    LS_EQ_INT(LS_GPS_DO_CONFIGURE, ls_gps_link_step(&link, true, 0, false));
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, true, 0, true));
}

LS_CASE(a_module_that_will_not_move_is_read_at_9600_unconfigured)
{
    ls_gps_link_t link = LS_GPS_LINK_TRY_9600;
    LS_EQ_INT(LS_GPS_DO_RAISE, ls_gps_link_step(&link, true, 300000, false));
    LS_EQ_INT(LS_GPS_DO_STAY_9600, step_quiet(&link, LS_GPS_CONFIRM_US, false));
    LS_EQ_INT(LS_GPS_LINK_AT_9600, link);
    /* No save and no configuration: 9600 cannot carry three constellations
       at once a second. */
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, true, 0, false));
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, false, 60000000, false));
}

LS_CASE(silence_at_both_rates_keeps_listening_at_115200)
{
    ls_gps_link_t link = LS_GPS_LINK_PROBE;
    LS_EQ_INT(LS_GPS_DO_LISTEN_9600, step_quiet(&link, LS_GPS_PROBE_US, false));
    LS_EQ_INT(LS_GPS_DO_NOTHING,
              ls_gps_link_step(&link, false, LS_GPS_TRY_9600_US - 1, false));
    LS_EQ_INT(LS_GPS_DO_GIVE_UP,
              ls_gps_link_step(&link, false, LS_GPS_TRY_9600_US, false));
    LS_EQ_INT(LS_GPS_LINK_SILENT, link);
    LS_EQ_INT(LS_GPS_DO_NOTHING, ls_gps_link_step(&link, false, 600000000, false));
    LS_EQ_INT(LS_GPS_LINK_SILENT, link);

    /* If it ever talks at 115200 it is configured then, once. */
    LS_EQ_INT(LS_GPS_DO_CONFIGURE, ls_gps_link_step(&link, true, 0, false));
    LS_EQ_INT(LS_GPS_LINK_UP, link);
}

LS_CASE(the_casic_commands_carry_their_own_checksums)
{
    /* The module ignores a command whose checksum is wrong, and says
       nothing about it. */
    static const char *const CMD[] = {
        LS_GPS_CMD_GNSS, LS_GPS_CMD_RATE_1HZ, LS_GPS_CMD_115200, LS_GPS_CMD_SAVE,
    };
    for (unsigned i = 0; i < sizeof(CMD) / sizeof(CMD[0]); i++) {
        const int len = (int)strlen(CMD[i]);
        LS_CHECK_MSG(len > 2 && CMD[i][len - 2] == '\r' && CMD[i][len - 1] == '\n',
                     "command %u is not CR LF terminated", i);
        ls_gps_parser_t p;
        fresh(&p);
        LS_CHECK_MSG(ls_gps_parse_line(CMD[i], len - 2, &p),
                     "command %u fails its checksum: %s", i, CMD[i]);
    }
    LS_EQ_STR("$PCAS04,7*1E\r\n", LS_GPS_CMD_GNSS);
    LS_EQ_STR("$PCAS02,1000*2E\r\n", LS_GPS_CMD_RATE_1HZ);
    LS_EQ_STR("$PCAS01,5*19\r\n", LS_GPS_CMD_115200);
    LS_EQ_STR("$PCAS00*01\r\n", LS_GPS_CMD_SAVE);
}
