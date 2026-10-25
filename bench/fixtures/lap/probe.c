/* Compiler/runtime fixture only: exercise soft double and 64-bit libgcc helpers. */
#include "lakeshark_app.h"
#include <stdio.h>
#include <math.h>
static volatile double d=1.234;
static volatile unsigned long long n=123456789012345ull, divisor=17;
static void enter(void)
{
    char *s=malloc(96);
    if(s) { snprintf(s,96,"%.6f %llu %.3f",d/3.14,n/divisor,(double)sinf((float)d)); lap_log("%s",s); free(s); }
}
LAP_ICON_PX("f..................0",
            "11111111111111111111",
            "22222222222222222222",
            "33333333333333333333",
            "44444444444444444444",
            "55555555555555555555",
            "66666666666666666666",
            "77777777777777777777",
            "88888888888888888888",
            "99999999999999999999",
            "aaaaaaaaaaaaaaaaaaaa",
            "bbbbbbbbbbbbbbbbbbbb",
            "cccccccccccccccccccc",
            "dddddddddddddddddddd",
            "eeeeeeeeeeeeeeeeeeee",
            "ffffffffffffffffffff",
            "00000000000000000000",
            "....................");
LAP_APP(.id="probe",.name="PROBE",.version="1.0",.author="bench",.icon=LAP_ICON_CHIP,.color=LAP_GREEN,.enter=enter);
