#ifndef P25_PROFILE_FIXTURE_H
#define P25_PROFILE_FIXTURE_H

static const char P25_PROFILE_MINIMAL_FIXTURE[] =
    "version=1\n"
    "system=Example System\n"
    "site=Example Site\n"
    "control=851012500\n";

static const char P25_PROFILE_FULL_FIXTURE[] =
    "# fictional test data\r\n"
    "version=1\r\n"
    "system=Example Regional Radio\r\n"
    "site=North Training Site\r\n"
    "control=851012500\r\n"
    "control=852237500\r\n"
    "preferred=852237500\r\n"
    "auto_follow=false\r\n"
    "encrypted_skip=false\r\n"
    "encrypted_skip_ms=45000\r\n"
    "demod=cqpsk\r\n"
    "cqpsk_timing_gain=0.0003125\r\n"
    "cqpsk_carrier_gain=0.02\r\n"
    "; roster follows\r\n"
    "tg=1201|Operations|true|7\r\n"
    "tg=1202|Facilities|false|0";

#endif
