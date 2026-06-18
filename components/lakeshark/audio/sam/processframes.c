#include "render.h"

extern unsigned char speed;

extern unsigned char multtable[];
extern unsigned char sinus[];
extern unsigned char rectangle[];

extern unsigned char pitches[256];
extern unsigned char sampledConsonantFlag[256];
extern unsigned char amplitude1[256];
extern unsigned char amplitude2[256];
extern unsigned char amplitude3[256];
extern unsigned char frequency1[256];
extern unsigned char frequency2[256];
extern unsigned char frequency3[256];

extern void Output(int index, unsigned char A);

static void CombineGlottalAndFormants(unsigned char phase1, unsigned char phase2, unsigned char phase3, unsigned char Y)
{
    unsigned int tmp;

    tmp   = multtable[sinus[phase1]     | amplitude1[Y]];
    tmp  += multtable[sinus[phase2]     | amplitude2[Y]];
    tmp  += tmp > 255 ? 1 : 0;
    tmp  += multtable[rectangle[phase3] | amplitude3[Y]];
    tmp  += 136;
    tmp >>= 4;

    Output(0, tmp & 0xf);
}

void ProcessFrames(unsigned char mem48)
{
    unsigned char speedcounter = 72;
	unsigned char phase1 = 0;
    unsigned char phase2 = 0;
	unsigned char phase3 = 0;
    unsigned char mem66 = 0;

    unsigned char Y = 0;

    unsigned char glottal_pulse = pitches[0];
    unsigned char mem38 = glottal_pulse - (glottal_pulse >> 2);

	while(mem48) {
		unsigned char flags = sampledConsonantFlag[Y];

        if(flags & 248) {
			RenderSample(&mem66, flags,Y);

			Y += 2;
			mem48 -= 2;
            speedcounter = speed;
		} else {
            CombineGlottalAndFormants(phase1, phase2, phase3, Y);

			speedcounter--;
			if (speedcounter == 0) {
                Y++;

                mem48--;
                if(mem48 == 0) return;
                speedcounter = speed;
            }

            --glottal_pulse;

            if(glottal_pulse != 0) {

                --mem38;

                if((mem38 != 0) || (flags == 0)) {

                    phase1 += frequency1[Y];
                    phase2 += frequency2[Y];
                    phase3 += frequency3[Y];
                    continue;
                }

                RenderSample(&mem66, flags,Y);
            }
        }

        glottal_pulse = pitches[Y];
        mem38 = glottal_pulse - (glottal_pulse>>2);

        phase1 = 0;
        phase2 = 0;
        phase3 = 0;
	}
}
