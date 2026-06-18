#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "render.h"
#include "RenderTabs.h"

#include "debug.h"
extern int debug;

extern unsigned char speed;
extern unsigned char pitch;
extern int singmode;

extern unsigned char phonemeIndexOutput[60];
extern unsigned char stressOutput[60];
extern unsigned char phonemeLengthOutput[60];

unsigned char pitches[256];

unsigned char frequency1[256];
unsigned char frequency2[256];
unsigned char frequency3[256];

unsigned char amplitude1[256];
unsigned char amplitude2[256];
unsigned char amplitude3[256];

unsigned char sampledConsonantFlag[256];

void AddInflection(unsigned char mem48, unsigned char X);

unsigned char trans(unsigned char a, unsigned char b)
{
    return (((unsigned int)a * b) >> 8) << 1;
}

extern int bufferpos;
extern char *buffer;

static const int timetable[5][5] =
{
	{162, 167, 167, 127, 128},
	{226, 60, 60, 0, 0},
	{225, 60, 59, 0, 0},
	{200, 0, 0, 54, 55},
	{199, 0, 0, 54, 54}
};

void Output(int index, unsigned char A)
{
	static unsigned oldtimetableindex = 0;
	int k;
	bufferpos += timetable[oldtimetableindex][index];
	oldtimetableindex = index;

	for(k=0; k<5; k++)
		buffer[bufferpos/50 + k] = (A & 15)*16;
}

static unsigned char RenderVoicedSample(unsigned short hi, unsigned char off, unsigned char phase1)
{
	do {
		unsigned char bit = 8;
		unsigned char sample = sampleTable[hi+off];
		do {
			if ((sample & 128) != 0) Output(3, 26);
			else Output(4, 6);
			sample <<= 1;
		} while(--bit != 0);
		off++;
	} while (++phase1 != 0);
	return off;
}

static void RenderUnvoicedSample(unsigned short hi, unsigned char off, unsigned char mem53)
{
    do {
        unsigned char bit = 8;
        unsigned char sample = sampleTable[hi+off];
        do {
            if ((sample & 128) != 0) Output(2, 5);
            else Output(1, mem53);
            sample <<= 1;
        } while (--bit != 0);
    } while (++off != 0);
}

void RenderSample(unsigned char *mem66, unsigned char consonantFlag, unsigned char mem49)
{

	unsigned char hibyte = (consonantFlag & 7)-1;

    unsigned short hi = hibyte*256;

	unsigned char pitchl = consonantFlag & 248;
	if(pitchl == 0) {

		pitchl = pitches[mem49] >> 4;
        *mem66 = RenderVoicedSample(hi, *mem66, pitchl ^ 255);
	}
	else
		RenderUnvoicedSample(hi, pitchl^255, tab48426[hibyte]);
}

static void CreateFrames()
{
	unsigned char X = 0;
    unsigned int i = 0;
    while(i < 256) {

        unsigned char phoneme = phonemeIndexOutput[i];
		unsigned char phase1;
		unsigned phase2;

        if (phoneme == 255) break;

        if (phoneme == PHONEME_PERIOD)   AddInflection(RISING_INFLECTION, X);
        else if (phoneme == PHONEME_QUESTION) AddInflection(FALLING_INFLECTION, X);

        phase1 = tab47492[stressOutput[i] + 1];

        phase2 = phonemeLengthOutput[i];

        do {
            frequency1[X] = freq1data[phoneme];
            frequency2[X] = freq2data[phoneme];
            frequency3[X] = freq3data[phoneme];
            amplitude1[X] = ampl1data[phoneme];
            amplitude2[X] = ampl2data[phoneme];
            amplitude3[X] = ampl3data[phoneme];
            sampledConsonantFlag[X] = sampledConsonantFlags[phoneme];
            pitches[X] = pitch + phase1;
            ++X;
        } while(--phase2 != 0);

        ++i;
    }
}

void RescaleAmplitude()
{
    int i;
    for(i=255; i>=0; i--)
        {
            amplitude1[i] = amplitudeRescale[amplitude1[i]];
            amplitude2[i] = amplitudeRescale[amplitude2[i]];
            amplitude3[i] = amplitudeRescale[amplitude3[i]];
        }
}

void AssignPitchContour()
{
    int i;
    for(i=0; i<256; i++) {

        pitches[i] -= (frequency1[i] >> 1);
    }
}

void Render()
{
    unsigned char t;

	if (phonemeIndexOutput[0] == 255) return;

    CreateFrames();
    t = CreateTransitions();

    if (!singmode) AssignPitchContour();
    RescaleAmplitude();

    if (debug) {
        PrintOutput(sampledConsonantFlag, frequency1, frequency2, frequency3, amplitude1, amplitude2, amplitude3, pitches);
    }

    ProcessFrames(t);
}

void AddInflection(unsigned char inflection, unsigned char pos)
{
    unsigned char A;

	unsigned char end = pos;

    if (pos < 30) pos = 0;
    else pos -= 30;

	while( (A = pitches[pos]) == 127) ++pos;

    while (pos != end) {

        A += inflection;

        pitches[pos] = A;

        while ((++pos != end) && pitches[pos] == 255);
    }
}

void SetMouthThroat(unsigned char mouth, unsigned char throat)
{

	static const unsigned char mouthFormants5_29[30] = {
		0, 0, 0, 0, 0, 10,
		14, 19, 24, 27, 23, 21, 16, 20, 14, 18, 14, 18, 18,
		16, 13, 15, 11, 18, 14, 11, 9, 6, 6, 6};

	static const unsigned char throatFormants5_29[30] = {
	255, 255,
	255, 255, 255, 84, 73, 67, 63, 40, 44, 31, 37, 45, 73, 49,
	36, 30, 51, 37, 29, 69, 24, 50, 30, 24, 83, 46, 54, 86,
    };

	static const unsigned char mouthFormants48_53[6] = {19, 27, 21, 27, 18, 13};

	static const unsigned char throatFormants48_53[6] = {72, 39, 31, 43, 30, 34};

	unsigned char newFrequency = 0;
	unsigned char pos = 5;

	while(pos < 30)
	{

		unsigned char initialFrequency = mouthFormants5_29[pos];
		if (initialFrequency != 0) newFrequency = trans(mouth, initialFrequency);
		freq1data[pos] = newFrequency;

		initialFrequency = throatFormants5_29[pos];
		if(initialFrequency != 0) newFrequency = trans(throat, initialFrequency);
		freq2data[pos] = newFrequency;
		pos++;
	}

	pos = 0;
    while(pos < 6) {

		unsigned char initialFrequency = mouthFormants48_53[pos];
		freq1data[pos+48] = trans(mouth, initialFrequency);

		initialFrequency = throatFormants48_53[pos];
		freq2data[pos+48] = trans(throat, initialFrequency);
		pos++;
	}
}
