#include <stdio.h>
#include <stdlib.h>
#include "render.h"

extern unsigned char phonemeIndexOutput[60];
extern unsigned char phonemeLengthOutput[60];

extern unsigned char blendRank[];
extern unsigned char outBlendLength[];
extern unsigned char inBlendLength[];
extern unsigned char pitches[];

extern unsigned char frequency1[256];
extern unsigned char frequency2[256];
extern unsigned char frequency3[256];

extern unsigned char amplitude1[256];
extern unsigned char amplitude2[256];
extern unsigned char amplitude3[256];

unsigned char Read(unsigned char p, unsigned char Y)
{
	switch(p)
	{
	case 168: return pitches[Y];
	case 169: return frequency1[Y];
	case 170: return frequency2[Y];
	case 171: return frequency3[Y];
	case 172: return amplitude1[Y];
	case 173: return amplitude2[Y];
	case 174: return amplitude3[Y];
	default:
		printf("Error reading from tables");
		return 0;
	}
}

void Write(unsigned char p, unsigned char Y, unsigned char value)
{
	switch(p)
	{
	case 168: pitches[Y]    = value; return;
	case 169: frequency1[Y] = value; return;
	case 170: frequency2[Y] = value; return;
	case 171: frequency3[Y] = value; return;
	case 172: amplitude1[Y] = value; return;
	case 173: amplitude2[Y] = value; return;
	case 174: amplitude3[Y] = value; return;
	default:
		printf("Error writing to tables\n");
		return;
	}
}

void interpolate(unsigned char width, unsigned char table, unsigned char frame, char mem53)
{
    unsigned char sign      = (mem53 < 0);
    unsigned char remainder = abs(mem53) % width;
    unsigned char div       = mem53 / width;

    unsigned char error = 0;
    unsigned char pos   = width;
    unsigned char val   = Read(table, frame) + div;

    while(--pos) {
        error += remainder;
        if (error >= width) {
            error -= width;
            if (sign) val--;
            else if (val) val++;
        }
        Write(table, ++frame, val);
        val += div;
    }
}

void interpolate_pitch(unsigned char pos, unsigned char mem49, unsigned char phase3) {

    unsigned char cur_width  = phonemeLengthOutput[pos] / 2;
    unsigned char next_width = phonemeLengthOutput[pos+1] / 2;

    unsigned char width = cur_width + next_width;
    char pitch = pitches[next_width + mem49] - pitches[mem49- cur_width];
    interpolate(width, 168, phase3, pitch);
}

unsigned char CreateTransitions()
{
	unsigned char mem49 = 0;
	unsigned char pos = 0;
	while(1) {
		unsigned char next_rank;
		unsigned char rank;
		unsigned char speedcounter;
		unsigned char phase1;
		unsigned char phase2;
		unsigned char phase3;
		unsigned char transition;

		unsigned char phoneme      = phonemeIndexOutput[pos];
		unsigned char next_phoneme = phonemeIndexOutput[pos+1];

		if (next_phoneme == 255) break;

		next_rank = blendRank[next_phoneme];
		rank      = blendRank[phoneme];

		if (rank == next_rank) {

			phase1 = outBlendLength[phoneme];
			phase2 = outBlendLength[next_phoneme];
		} else if (rank < next_rank) {

			phase1 = inBlendLength[next_phoneme];
			phase2 = outBlendLength[next_phoneme];
		} else {

			phase1 = outBlendLength[phoneme];
			phase2 = inBlendLength[phoneme];
		}

		mem49 += phonemeLengthOutput[pos];

		speedcounter = mem49 + phase2;
		phase3       = mem49 - phase1;
		transition   = phase1 + phase2;

		if (((transition - 2) & 128) == 0) {
            unsigned char table = 169;
            interpolate_pitch(pos, mem49, phase3);
            while (table < 175) {

                char value = Read(table, speedcounter) - Read(table, phase3);
                interpolate(transition, table, phase3, value);
                table++;
            }
        }
		++pos;
	}

    return mem49 + phonemeLengthOutput[pos];
}
