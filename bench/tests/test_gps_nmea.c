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
static bool feed(ls_gps_state_t *st, const char *line)
{
    return ls_gps_parse_line(line, (int)strlen(line), st);
}

static void fresh(ls_gps_state_t *st) { memset(st, 0, sizeof(*st)); }

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_bad_checksum_changes_nothing)
{
    /* The first line of defence. A sentence damaged in transit must not move
       a position, and at 115200 on a shared board damage happens. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    double lat = st.lat_deg, lon = st.lon_deg;

    /* Same sentence with one digit changed and a checksum that does not
       match it. The value is deliberately wrong: a helper that recomputes
       checksums across this file must leave this one alone. */
    LS_CHECK_MSG(!feed(&st, "$GPGGA,123519,4907.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*B9"),
                 "a sentence with a broken checksum was accepted");
    LS_CHECK_MSG(st.lat_deg == lat && st.lon_deg == lon,
                 "a rejected sentence still moved the position");
}

LS_CASE(degrees_and_minutes_become_decimal_degrees)
{
    /* The conversion that has never run. 4807.038 N is 48 degrees and 7.038
       minutes: 48 + 7.038/60 = 48.1173. Getting this wrong by leaving it as
       4807.038, or as 48.07038, puts the device somewhere impossible or a
       few kilometres out, and the second is the one nobody notices. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));

    LS_CHECK(st.fix);
    LS_NEAR(48.1173, st.lat_deg, 0.0001);
    LS_NEAR(11.5167, st.lon_deg, 0.0001);
}

LS_CASE(longitude_has_three_degree_digits_and_latitude_two)
{

    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_CHECK_MSG(st.lon_deg > 11.0 && st.lon_deg < 12.0,
                 "longitude parsed as %f, expected about 11.52", st.lon_deg);

    /* And a three-digit longitude past 100 degrees still works. */
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,001043.00,4404.14036,N,12118.85961,W,1,12,0.98,1113.0,M,-21.3,M,,*59"));
    LS_NEAR(44.0690, st.lat_deg, 0.001);
    LS_NEAR(-121.3143, st.lon_deg, 0.001);
}

LS_CASE(south_and_west_are_negative)
{
    /* Sydney: south and east. A missing sign puts it in Russia. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,062101,3352.000,S,15112.000,E,1,09,0.8,20.0,M,20.0,M,,*6B"));
    LS_CHECK_MSG(st.lat_deg < 0, "a southern latitude came out as %f", st.lat_deg);
    LS_CHECK_MSG(st.lon_deg > 0, "an eastern longitude came out as %f", st.lon_deg);
    LS_NEAR(-33.8667, st.lat_deg, 0.001);
    LS_NEAR(151.2000, st.lon_deg, 0.001);
}

LS_CASE(an_empty_position_is_not_a_fix_at_the_equator)
{
    /* What the board has actually been producing all along: a framed sentence
       with nothing in it. The dangerous failure is treating those empty
       fields as zero and reporting a confident fix in the Gulf of Guinea. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E"));

    LS_CHECK_MSG(!st.fix, "an empty GGA was reported as a fix");
    LS_EQ_INT(0, st.quality);
    LS_EQ_INT(0, st.sats_used);
}

LS_CASE(losing_a_fix_clears_it)
{
    /* A receiver that goes indoors keeps sending, with quality back to 0.
       Leaving `fix` true would show a stale position as current. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_CHECK(st.fix);

    LS_CHECK(feed(&st, "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E"));
    LS_CHECK_MSG(!st.fix, "the fix survived a quality of zero");
}

LS_CASE(rmc_carries_a_position_a_date_and_a_speed)
{
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A"));

    LS_CHECK(st.fix);
    LS_NEAR(48.1173, st.lat_deg, 0.0001);
    LS_NEAR(22.4, st.speed_kts, 0.1);
    LS_NEAR(84.4, st.course_deg, 0.1);
    LS_EQ_INT(23, st.day);
    LS_EQ_INT(3, st.month);
    LS_EQ_INT(1994, st.year);
}

LS_CASE(an_rmc_marked_void_does_not_place_the_receiver)
{
    /* Status V means the data is not valid. Reading the coordinates anyway
       is how a receiver that is still searching reports a position. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*7D"));
    LS_CHECK_MSG(!st.fix, "a void RMC was treated as a fix");
}

LS_CASE(the_clock_comes_out_of_the_time_field)
{
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_EQ_INT(12, st.hour);
    LS_EQ_INT(35, st.minute);
    LS_EQ_INT(19, st.second);

    /* Fractional seconds are common and must not shift the fields. */
    fresh(&st);
    LS_CHECK(feed(&st, "$GNGGA,044750.600,,,,,0,00,25.5,,,,,,*7E"));
    LS_EQ_INT(4, st.hour);
    LS_EQ_INT(47, st.minute);
    LS_EQ_INT(50, st.second);
}

LS_CASE(satellites_in_view_are_summed_across_constellations_once_a_cycle)
{
    /* Each constellation sends its own GSV series and repeats its count in
       every message of that series, so only the first of each may be added.
       GGA closes the cycle and publishes. */
    ls_gps_state_t st;
    fresh(&st);

    /* GPS: 11 in view, over three messages. */
    LS_CHECK(feed(&st, "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74"));
    LS_CHECK(feed(&st, "$GPGSV,3,2,11,14,25,170,00,16,57,208,39,18,67,296,40,19,40,246,00*74"));
    /* GLONASS: 4 more. */
    LS_CHECK(feed(&st, "$GLGSV,1,1,04,65,10,120,00,66,45,230,35,67,20,300,28,68,05,050,00*62"));

    /* Nothing published until a cycle closes. */
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    LS_CHECK_MSG(st.sats_visible == 15,
                 "expected 11 + 4 satellites in view, got %d", st.sats_visible);

    /* A second series repeated in the same message must not double count. */
    LS_EQ_INT(0, st.sats_acc);
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
        ls_gps_state_t st;
        fresh(&st);
        LS_CHECK_MSG(feed(&st, SAME[i]), "sentence %u was rejected", i);
        LS_CHECK(st.fix);
        LS_NEAR(48.1173, st.lat_deg, 0.0001);
    }
}

LS_CASE(a_sentence_the_parser_does_not_know_is_ignored_quietly)
{
    /* A receiver emits VTG, GLL, GSA and more. They are not errors and must
       not disturb what the known ones established. */
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(feed(&st, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    double lat = st.lat_deg;

    LS_CHECK(feed(&st, "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48"));
    LS_CHECK(feed(&st, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39"));
    LS_CHECK_MSG(st.lat_deg == lat, "an unknown sentence moved the position");
    LS_CHECK(st.fix);
}

LS_CASE(rubbish_is_refused_rather_than_parsed)
{
    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(!feed(&st, ""));
    LS_CHECK(!feed(&st, "$"));
    LS_CHECK(!feed(&st, "$GPGGA"));
    LS_CHECK(!feed(&st, "not a sentence at all"));
    LS_CHECK(!feed(&st, "$GPGGA,123519,4807.038,N*"));
    LS_CHECK(!feed(&st, "$GPGGA,123519,4807.038,N*ZZ"));
    LS_CHECK(!ls_gps_parse_line(NULL, 10, &st));
    LS_CHECK(!ls_gps_parse_line("$GPGGA*56", 9, NULL));
    LS_CHECK_MSG(!st.fix, "rubbish produced a fix");
}

LS_CASE(a_sentence_longer_than_nmea_allows_is_refused)
{
    /* 82 bytes is the limit. Anything longer is a framing fault, and a parser
       that copies it into a fixed buffer without checking is the classic
       overflow. */
    ls_gps_state_t st;
    fresh(&st);
    char big[200];
    memset(big, 'A', sizeof(big));
    big[0] = '$';
    memcpy(big + sizeof(big) - 3, "*00", 3);
    LS_CHECK(!ls_gps_parse_line(big, (int)sizeof(big), &st));
}

LS_CASE(the_input_sentence_is_not_modified)
{
    /* The reader hands the same buffer to the parser and then reuses it. A
       parser that writes terminators into its input would corrupt the next
       sentence assembled there. */
    char line[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    char copy[sizeof(line)];
    memcpy(copy, line, sizeof(line));

    ls_gps_state_t st;
    fresh(&st);
    LS_CHECK(ls_gps_parse_line(line, (int)strlen(line), &st));
    LS_CHECK_MSG(memcmp(line, copy, sizeof(line)) == 0,
                 "the parser modified the sentence it was given");
}
