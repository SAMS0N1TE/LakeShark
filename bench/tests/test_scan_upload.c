#include "ls_test.h"
#include "../../components/lakeshark/core/scan_import_file.c"
static int saved_count, stopped;
static bool write_fail;
bool scan_channels_replace(const scan_channel_t *c,int n) {
    if (write_fail) return false;
    saved_count=n; return true;
}
void scan_engine_stop(void) { ++stopped; }
esp_err_t ls_nvs_call(ls_nvs_fn_t fn,void *ctx,unsigned stack) { return fn(ctx); }
static const char *valid="Test|154785000|P25|0|43.4|-71.6|25|0";
LS_CASE(upload_rejects_the_whole_transaction_after_a_bad_row)
{
    saved_count=7; stopped=0; scan_import_begin();
    LS_CHECK(scan_import_add(valid)); LS_CHECK(!scan_import_add("invalid"));
    char message[96]; LS_CHECK(!scan_import_commit(message,sizeof(message)));
    LS_EQ_INT(saved_count,7); LS_EQ_INT(stopped,0);
}
LS_CASE(upload_commits_once_and_expires_abandoned_staging)
{
    saved_count=0; stopped=0; write_fail=false; ls_shim_time_set(0);
    scan_import_begin(); LS_CHECK(scan_import_add(valid));
    char message[96]; LS_CHECK(scan_import_commit(message,sizeof(message)));
    LS_EQ_INT(saved_count,1); LS_EQ_INT(stopped,1);
    scan_import_begin(); LS_CHECK(scan_import_add(valid));
    ls_shim_time_advance(61000000);
    LS_CHECK(!scan_import_commit(message,sizeof(message))); LS_EQ_INT(stopped,1);
}
LS_CASE(file_import_rejects_embedded_nulls_and_duplicate_rows)
{
    const char *path="scan_upload_fixture.tmp";
    FILE *f=fopen(path,"wb"); LS_CHECK(f!=NULL);
    fputs("LSCAN1\n",f); fputs(valid,f); fputc(0,f); fputs("junk\n",f); fclose(f);
    char message[96]; request_t r={path,message,sizeof(message)};
    saved_count=7; LS_CHECK(import_worker(&r)!=ESP_OK); LS_EQ_INT(saved_count,7);
    f=fopen(path,"wb"); LS_CHECK(f!=NULL);
    fprintf(f,"LSCAN1\n%s\n%s\n",valid,valid); fclose(f);
    LS_CHECK(import_worker(&r)!=ESP_OK); LS_EQ_INT(saved_count,7);
    remove(path);
}
