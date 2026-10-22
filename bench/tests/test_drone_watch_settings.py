"""Execute the actual preference accessors with a bounded fake write queue."""
import unittest
from test_audio_reinit import ROOT, body, run_c


class DroneSettings(unittest.TestCase):
    def test_version_validation_deferred_queue_and_failed_write_cache(self):
        source = (ROOT / 'components/lakeshark/core/settings.c').read_text()
        schema = (ROOT / 'components/lakeshark/core/settings_schema.c').as_posix()
        run_c(r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define CHECK(x) do {if(!(x))return __LINE__;} while(0)
''' + '#include "' + schema + '"\n' + r'''
static uint32_t s_drone_watch=SETTINGS_DRONE_WATCH_DEFAULT;
static void *s_wq;
static unsigned writes;
static bool accepted=true;
static bool sput_u32(const char *key,uint32_t value) {
    if(strcmp(key,"drone_watch_v1") || !settings_drone_watch_valid(value))return false;
    writes++;return accepted;
}
''' + body(source, 'uint32_t settings_get_drone_watch(void)') + '\n' +
              body(source, 'bool settings_set_drone_watch(uint32_t value)') + r'''
int main(void) {
    CHECK(!settings_set_drone_watch(0x10000));CHECK(writes==0);
    s_wq=(void *)1;CHECK(!settings_set_drone_watch(0x200ff));CHECK(writes==0);
    accepted=false;CHECK(!settings_set_drone_watch(0x10000));CHECK(settings_get_drone_watch()==0x100fe);
    accepted=true;CHECK(settings_set_drone_watch(0x10000));CHECK(settings_get_drone_watch()==0x10000);
    CHECK(writes==2);return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
