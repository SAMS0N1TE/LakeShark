#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void iq_test_critical_enter(portMUX_TYPE *mux);
void iq_test_critical_exit(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) iq_test_critical_enter(mux)
#define portEXIT_CRITICAL(mux) iq_test_critical_exit(mux)
