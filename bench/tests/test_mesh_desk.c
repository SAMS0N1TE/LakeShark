/* MESH on the desk, the part with no screen in it: which conversations the
   message ring and the peer list make, what is unread in each, how a line
   wraps, and the node table in every order. */
#include "ls_test.h"
#include "ls_mesh_desk.h"

#include <string.h>

static ls_mesh_msg_t msg(uint32_t t, bool mine, const char *peer, int chan, const char *text)
{
    ls_mesh_msg_t m;
    memset(&m, 0, sizeof(m));
    m.t = t;
    m.mine = mine;
    m.direct = peer != NULL;
    if (peer) strcpy(m.peer, peer);
    m.chan = (uint8_t)chan;
    m.state = mine ? LS_MSG_SENT : LS_MSG_IN;
    strcpy(m.text, text);
    return m;
}

static ls_mesh_peer_t peer(const char *id, const char *name, int type, uint32_t heard, float rssi, float snr)
{
    ls_mesh_peer_t p;
    memset(&p, 0, sizeof(p));
    strcpy(p.id, id);
    strcpy(p.name, name);
    p.type = (uint8_t)type;
    p.last_heard = heard;
    p.first_heard = heard ? heard - 100 : 0;
    p.rssi = rssi;
    p.snr = snr;
    return p;
}

#define KB "17BC5E90A3D46F82"
#define NF "A41F09D7C2B8E350"
#define GONE "00000000DEADBEEF"

static ls_mesh_chan_t CH[3] = { { "Public", "x", true }, { "LAKE", "abcd", false }, { "", "", false } };

LS_CASE(conversations_are_channels_then_nodes)
{
    ls_mesh_msg_t m[] = {
        msg(100, false, NULL, 0, "NORTHFIELD: up on the hill"),
        msg(110, true, NULL, 0, "SHARK: copy"),
        msg(120, false, GONE, 0, "OLD: are you there"),
        msg(130, false, NULL, 1, "TILTON: net at seven"),
        msg(140, false, KB, 0, "KB1QWE: on the lake today"),
        msg(150, true, KB, 0, "SHARK: yes"),
    };
    ls_mesh_peer_t p[] = { peer(NF, "NORTHFIELD", LS_MESH_ROLE_REPEATER, 90, -74, 11),
                           peer(KB, "KB1QWE", LS_MESH_ROLE_CHAT, 95, -88, 7) };
    ls_meshd_conv_t c[12];
    ls_meshd_read_t read = { 0 };
    const int n = ls_meshd_convs(c, 12, m, 6, CH, 3, p, 2, &read);
    /* Public, LAKE (slot 2 has no key), then the DMs newest first, then the node with none. */
    LS_EQ_INT(n, 5);
    LS_EQ_INT(c[0].kind, LS_MC_PUBLIC);
    LS_EQ_STR(c[0].name, "Public");
    LS_EQ_INT(c[0].total, 2);
    LS_EQ_INT(c[0].unread, 1);
    LS_EQ_INT(c[1].kind, LS_MC_CHANNEL);
    LS_EQ_STR(c[1].name, "LAKE");
    LS_EQ_INT(c[1].unread, 1);
    LS_EQ_STR(c[2].id, KB);
    LS_EQ_STR(c[2].name, "KB1QWE");
    LS_CHECK(c[2].heard);
    LS_EQ_INT(c[2].total, 2);
    LS_EQ_INT(c[2].unread, 1);
    LS_EQ_INT((int)c[2].last_t, 150);
    /* A node that wrote and is not in the peer list: named by its id, not addressable. */
    LS_EQ_STR(c[3].id, GONE);
    LS_EQ_STR(c[3].name, "00000000");
    LS_CHECK(!c[3].heard);
    LS_EQ_STR(c[4].name, "NORTHFIELD");
    LS_EQ_INT(c[4].total, 0);
    LS_EQ_INT(c[4].role, LS_MESH_ROLE_REPEATER);
    /* A message is in one conversation and no other. */
    for (int i = 0; i < 6; i++) {
        int in = 0;
        for (int k = 0; k < n; k++) in += ls_meshd_belongs(&m[i], &c[k]);
        LS_EQ_INT(in, 1);
    }
    /* Too little room: what fits, never more. */
    LS_EQ_INT(ls_meshd_convs(c, 2, m, 6, CH, 3, p, 2, &read), 2);
    /* A channel message on a slot whose key is gone still has somewhere to be read. */
    ls_mesh_msg_t stray = msg(160, false, NULL, 2, "X: hello");
    LS_EQ_INT(ls_meshd_convs(c, 12, &stray, 1, CH, 3, NULL, 0, NULL), 3);
    LS_EQ_STR(c[2].name, "Channel 2");
    LS_EQ_INT(c[2].unread, 0);                    /* no read marks asked for: nothing is unread */
}

LS_CASE(unread_is_what_arrived_since_it_was_looked_at)
{
    ls_mesh_msg_t m[8] = {
        msg(100, false, NULL, 0, "A: one"),
        msg(110, false, KB, 0, "KB1QWE: dm one"),
        msg(120, false, NULL, 0, "A: two"),
    };
    int n = 3;
    ls_meshd_conv_t pub = { .kind = LS_MC_PUBLIC }, dm = { .kind = LS_MC_DM };
    strcpy(dm.id, KB);
    ls_meshd_read_t read = { 0 };
    LS_EQ_INT(ls_meshd_unread(&read, &pub, m, n), 2);
    LS_EQ_INT(ls_meshd_unread(&read, &dm, m, n), 1);
    ls_meshd_read_all(&read, &pub, m, n);
    LS_EQ_INT(ls_meshd_unread(&read, &pub, m, n), 0);
    LS_EQ_INT(ls_meshd_unread(&read, &dm, m, n), 1);           /* reading one leaves the other */
    /* Ours never count; theirs do. */
    m[n++] = msg(130, true, NULL, 0, "SHARK: mine");
    LS_EQ_INT(ls_meshd_unread(&read, &pub, m, n), 0);
    m[n++] = msg(140, false, NULL, 0, "A: three");
    m[n++] = msg(150, false, NULL, 0, "A: four");
    LS_EQ_INT(ls_meshd_unread(&read, &pub, m, n), 2);
    /* The id in lower case is the same node. */
    ls_meshd_conv_t low = dm;
    strcpy(low.id, "17bc5e90a3d46f82");
    ls_meshd_read_all(&read, &low, m, n);
    LS_EQ_INT(ls_meshd_unread(&read, &dm, m, n), 0);
    LS_EQ_INT(read.n, 2);
    /* The ring moves on and the mark leaves it: what is left is all newer. */
    LS_EQ_INT(ls_meshd_unread(&read, &pub, m + 4, n - 4), 2);
    ls_meshd_read_all(&read, &pub, m, n);
    LS_EQ_INT(ls_meshd_unread(&read, &pub, m + 5, n - 5), 0);
    /* An empty conversation looked at, then written to. */
    ls_meshd_conv_t lake = { .kind = LS_MC_CHANNEL, .chan = 1 };
    ls_meshd_read_all(&read, &lake, m, n);
    m[n++] = msg(160, false, NULL, 1, "T: on lake");
    LS_EQ_INT(ls_meshd_unread(&read, &lake, m, n), 1);
    /* More conversations than marks: the table never overruns. */
    for (int i = 0; i < 2 * LS_MESHD_READ; i++) {
        ls_meshd_conv_t c = { .kind = LS_MC_CHANNEL, .chan = (uint8_t)(10 + i) };
        ls_meshd_read_all(&read, &c, m, n);
    }
    LS_EQ_INT(read.n, LS_MESHD_READ);
}

LS_CASE(a_message_is_split_into_who_and_what)
{
    char who[24];
    ls_mesh_msg_t m = msg(1, false, NULL, 0, "NORTHFIELD: up: on the hill");
    LS_EQ_STR(ls_meshd_split(&m, who, sizeof(who)), "up: on the hill");
    LS_EQ_STR(who, "NORTHFIELD");
    m = msg(1, false, NULL, 0, "no name in this one");
    LS_EQ_STR(ls_meshd_split(&m, who, sizeof(who)), "no name in this one");
    LS_EQ_STR(who, "");
    /* The message's own sender field wins, with or without the name still in the text. */
    m = msg(1, false, NULL, 0, "KB1QWE: hello");
    strcpy(m.sender, "KB1QWE");
    LS_EQ_STR(ls_meshd_split(&m, who, sizeof(who)), "hello");
    m = msg(1, false, NULL, 0, "see: this");
    strcpy(m.sender, "KB1QWE");
    LS_EQ_STR(ls_meshd_split(&m, who, sizeof(who)), "see: this");
    LS_EQ_STR(who, "KB1QWE");
    LS_EQ_STR(ls_meshd_split(&m, NULL, 0), "see: this");
    /* Find is case-blind over both. */
    LS_CHECK(ls_meshd_match(&m, "kb1"));
    LS_CHECK(ls_meshd_match(&m, "THIS"));
    LS_CHECK(ls_meshd_match(&m, ""));
    LS_CHECK(!ls_meshd_match(&m, "lake"));
    m = msg(1, true, NULL, 0, "x");
    m.state = LS_MSG_ACKED;
    LS_EQ_STR(ls_meshd_state_word(&m), "delivered");
    m.state = LS_MSG_FAILED;
    LS_EQ_STR(ls_meshd_state_word(&m), "failed");
    m.mine = false;
    LS_EQ_STR(ls_meshd_state_word(&m), "");
}

LS_CASE(lines_wrap_at_spaces_and_never_lose_a_character)
{
    const char *text = "A long incoming message keeps every byte and wraps across the feed so the end stays readable. FINALTAIL";
    char line[64], all[160] = "";
    int off = 0, rows = 0;
    do {
        off = ls_meshd_wrap(text, off, 20, line, sizeof(line));
        LS_CHECK(strlen(line) <= 20);
        LS_CHECK(line[0] != ' ');
        if (all[0]) strcat(all, " ");
        strcat(all, line);
        rows++;
    } while (text[off] && rows < 20);
    LS_EQ_STR(all, text);
    LS_CHECK(rows >= 5 && rows < 20);
    /* A word longer than the line is cut, not dropped. */
    off = ls_meshd_wrap("ABCDEFGHIJKLMNOP", 0, 5, line, sizeof(line));
    LS_EQ_STR(line, "ABCDE");
    LS_EQ_INT(off, 5);
    /* A UTF-8 letter is one cell and is never split. */
    off = ls_meshd_wrap("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82", 0, 3, line, sizeof(line));
    LS_EQ_STR(line, "\xD0\x9F\xD1\x80\xD0\xB8");
    LS_EQ_INT(off, 6);
    /* No line asked for: only the count. */
    LS_EQ_INT(ls_meshd_wrap("one two", 0, 3, NULL, 0), 4);
    off = ls_meshd_wrap("", 0, 10, line, sizeof(line));
    LS_EQ_INT(off, 0);
    LS_EQ_STR(line, "");
}

LS_CASE(nodes_sort_by_any_column_and_blanks_go_last)
{
    ls_mesh_peer_t p[] = {
        peer("AA00000000000001", "NORTHFIELD", LS_MESH_ROLE_REPEATER, 900, -74, 11.2f),
        peer("AA00000000000002", "KB1QWE", LS_MESH_ROLE_CHAT, 950, -88, 7.4f),
        peer("AA00000000000003", "", LS_MESH_ROLE_CHAT, 800, -113, -6.5f),
        peer("AA00000000000004", "WEIR TEMP", LS_MESH_ROLE_SENSOR, 0, 0, 0),     /* saved, never heard */
    };
    p[0].adverts = 61; p[1].adverts = 24; p[2].adverts = 2; p[3].adverts = 0;
    const float dist[] = { 5200, 800, -1, 12400 };
    uint8_t o[4];
    ls_meshd_sort(o, p, 4, LS_MN_NAME, false, dist);
    LS_CHECK(o[0] == 1 && o[1] == 0 && o[2] == 3 && o[3] == 2);         /* the unnamed one last */
    ls_meshd_sort(o, p, 4, LS_MN_NAME, true, dist);
    LS_CHECK(o[0] == 3 && o[1] == 0 && o[2] == 1 && o[3] == 2);         /* and last the other way too */
    ls_meshd_sort(o, p, 4, LS_MN_HEARD, false, dist);
    LS_CHECK(o[0] == 1 && o[1] == 0 && o[2] == 2 && o[3] == 3);         /* most recent first, never heard last */
    ls_meshd_sort(o, p, 4, LS_MN_HEARD, true, dist);
    LS_CHECK(o[0] == 2 && o[1] == 0 && o[2] == 1 && o[3] == 3);
    ls_meshd_sort(o, p, 4, LS_MN_RSSI, false, dist);
    LS_CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2 && o[3] == 3);         /* strongest first */
    ls_meshd_sort(o, p, 4, LS_MN_SNR, true, dist);
    LS_CHECK(o[0] == 2 && o[1] == 1 && o[2] == 0 && o[3] == 3);
    ls_meshd_sort(o, p, 4, LS_MN_DIST, false, dist);
    LS_CHECK(o[0] == 1 && o[1] == 0 && o[2] == 3 && o[3] == 2);         /* nearest first, no position last */
    ls_meshd_sort(o, p, 4, LS_MN_DIST, true, dist);
    LS_CHECK(o[0] == 3 && o[1] == 0 && o[2] == 1 && o[3] == 2);
    ls_meshd_sort(o, p, 4, LS_MN_DIST, false, NULL);
    LS_CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2 && o[3] == 3);         /* nothing to sort by: as heard */
    ls_meshd_sort(o, p, 4, LS_MN_ADVERTS, false, dist);
    LS_CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2 && o[3] == 3);
    ls_meshd_sort(o, p, 4, LS_MN_ROLE, false, dist);
    LS_CHECK(o[0] == 1 && o[1] == 2 && o[2] == 0 && o[3] == 3);         /* chat, chat, repeater, sensor */
    ls_meshd_sort(o, p, 4, LS_MN_ID, true, dist);
    LS_CHECK(o[0] == 3 && o[3] == 0);
    ls_meshd_sort(o, p, 0, LS_MN_NAME, false, dist);                     /* an empty list is no trouble */
}

LS_CASE(ranges_and_times_read_as_a_person_says_them)
{
    char t[24];
    ls_meshd_range_text(-1, t, sizeof(t)); LS_EQ_STR(t, "-");
    ls_meshd_range_text(849.6f, t, sizeof(t)); LS_EQ_STR(t, "850 m");
    ls_meshd_range_text(12440, t, sizeof(t)); LS_EQ_STR(t, "12.4 km");
    ls_meshd_range_text(250000, t, sizeof(t)); LS_EQ_STR(t, "250 km");
    ls_meshd_time_text(0, t, sizeof(t)); LS_EQ_STR(t, "--:--");
    ls_meshd_time_text(1757000000u, t, sizeof(t)); LS_EQ_STR(t, "15:33");      /* 2025-09-04 15:33:20 UTC */
    ls_meshd_time_text(125, t, sizeof(t)); LS_EQ_STR(t, "+2m05");              /* no clock: since the board started */
    ls_meshd_time_text(3 * 3600 + 120, t, sizeof(t)); LS_EQ_STR(t, "+3h02");
    LS_EQ_STR(ls_meshd_role(LS_MESH_ROLE_ROOM), "room");
    LS_EQ_STR(ls_meshd_role(99), "-");
}
