/* P25 DSP diagnostic sinks for the headless simulator and loader tests. */
void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }
#ifdef LS_LAP_HOST_TEST
void diag_line(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#endif
