/* SAM debug stubs — debug output disabled in embedded build */
int debug = 0;

void PrintPhonemes(unsigned char *phonemeindex, unsigned char *phonemeLength, unsigned char *stress) { (void)phonemeindex; (void)phonemeLength; (void)stress; }
void PrintOutput(unsigned char *flag, unsigned char *f1, unsigned char *f2, unsigned char *f3,
                 unsigned char *a1, unsigned char *a2, unsigned char *a3, unsigned char *p) {
    (void)flag; (void)f1; (void)f2; (void)f3; (void)a1; (void)a2; (void)a3; (void)p;
}
void PrintRule(unsigned short offset) { (void)offset; }
