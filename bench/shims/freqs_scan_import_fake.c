/* scan_import_file on the bench: the board's runs on the NVS worker and
   replaces the scanner's list. Here it reads the file with the scanner's own
   line parser and remembers what it would have loaded, so FREQS's LOAD INTO
   SCANNER can be checked without a scanner. */
#include "scan_import.h"

#include <stdio.h>
#include <string.h>

int freqs_fake_scanner_loaded = -1;
char freqs_fake_scanner_path[128];

bool scan_import_file(const char *path, char *message, size_t capacity)
{
    FILE *f = fopen(path, "r");
    char line[192];
    int n = 0;
    bool ok = f && fgets(line, sizeof(line), f) && !strncmp(line, "LSCAN1", 6);
    while (ok && fgets(line, sizeof(line), f)) {
        scan_channel_t ch;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!scan_import_line(line, &ch)) ok = false;
        else n++;
    }
    if (f) fclose(f);
    snprintf(freqs_fake_scanner_path, sizeof(freqs_fake_scanner_path), "%s", path);
    freqs_fake_scanner_loaded = ok ? n : -1;
    snprintf(message, capacity, ok ? "Imported %d channels; scan stopped" : "Rejected import", n);
    return ok;
}
