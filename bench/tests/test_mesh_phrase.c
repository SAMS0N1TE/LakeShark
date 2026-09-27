#include "ls_test.h"
#include "mesh_phrase.h"

LS_CASE(sender_only_names_who_sent_it)
{
    char out[192];
    mesh_phrase(out, sizeof(out), "Alice: are you on?", false, false);
    LS_EQ_STR(out, "MESSAGE FROM Alice.");
    mesh_phrase(out, sizeof(out), "Base-2: ok", true, false);
    LS_EQ_STR(out, "DIRECT MESSAGE FROM Base 2.");
}

LS_CASE(full_reads_the_message_and_ends_it)
{
    char out[192];
    mesh_phrase(out, sizeof(out), "Alice: are you on channel 3", false, true);
    LS_EQ_STR(out, "MESSAGE FROM Alice. are you on channel 3.");
    mesh_phrase(out, sizeof(out), "Bob: here!", false, true);
    LS_EQ_STR(out, "MESSAGE FROM Bob. here!");
}

LS_CASE(what_cannot_be_said_becomes_a_pause)
{
    char out[192];
    mesh_phrase(out, sizeof(out), "\xF0\x9F\x93\xA1 node: hi \xF0\x9F\x98\x80 there :)", false, true);
    LS_EQ_STR(out, "MESSAGE FROM node. hi there.");
    mesh_phrase(out, sizeof(out), "\xF0\x9F\x93\xA1: hi", false, false);
    LS_EQ_STR(out, "MESSAGE.");
    mesh_phrase(out, sizeof(out), "no name here", false, true);
    LS_EQ_STR(out, "MESSAGE. no name here.");
}

LS_CASE(machine_payloads_are_not_read_out)
{
    char out[192];
    mesh_phrase(out, sizeof(out), "SHARK -> LS Cell Gate: CW1:1u3awQIAAAB/MahqiF0NAMtMQgDqqJL/AAD//wAAVRs=",
                false, true);
    LS_EQ_STR(out, "MESSAGE FROM SHARK LS Cell Gate.");
}

LS_CASE(a_small_buffer_is_never_overrun)
{
    char out[12];
    mesh_phrase(out, sizeof(out), "Alexandria: a long message indeed", true, true);
    LS_CHECK(strlen(out) < sizeof(out));
    mesh_phrase(out, 1, "x: y", false, true);
    LS_EQ_STR(out, "");
}
