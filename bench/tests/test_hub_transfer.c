#include "ls_test.h"
#include "../../main/ls_hub_transfer.h"

LS_CASE(hub_crc_matches_standard_check_vector_and_chunking)
{
    const char *data = "123456789";
    LS_CHECK(ls_hub_crc32(0, data, 9) == 0xcbf43926u);
    LS_CHECK(ls_hub_crc32(ls_hub_crc32(0, data, 4), data + 4, 5) == 0xcbf43926u);
}

LS_CASE(hub_map_names_cannot_escape_the_map_directory)
{
    LS_CHECK(ls_hub_map_name("north-east.pmtiles"));
    LS_CHECK(!ls_hub_map_name("../north.pmtiles"));
    LS_CHECK(!ls_hub_map_name("a\\north.pmtiles"));
    LS_CHECK(!ls_hub_map_name(".pmtiles"));
    LS_CHECK(!ls_hub_map_name("north.bin"));
}

LS_CASE(hub_map_header_rejects_wrong_tiles_and_out_of_file_ranges)
{
    uint8_t header[127] = {0};
    memcpy(header, "PMTiles\003", 8);
    header[99] = 1; header[101] = 14;
    header[8] = 127; header[16] = 1;
    LS_CHECK(ls_hub_map_header(header, 128));
    LS_CHECK(!ls_hub_map_header(header, 127));
    header[99] = 2;
    LS_CHECK(!ls_hub_map_header(header, 128));
    header[99] = 1;
    memset(header + 8, 255, 8);
    LS_CHECK(!ls_hub_map_header(header, 128));
}
