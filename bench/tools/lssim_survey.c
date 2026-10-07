#include "ls_survey.h"
#include <string.h>
bool ls_survey_run(bool start, const ls_survey_options_t *options)
{ (void)start; (void)options; return false; }
void ls_survey_view(ls_survey_view_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->status, sizeof(out->status), "Survey radio unavailable on host");
}
bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out)
{ (void)rank; (void)sort; (void)out; return false; }
