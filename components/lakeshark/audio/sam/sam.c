#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "debug.h"
#include "sam.h"
#include "render.h"
#include "SamTabs.h"

enum {
    pR    = 23,
    pD    = 57,
    pT    = 69,
    BREAK = 254,
    END   = 255
};

unsigned char input[256];
unsigned char speed = 72;
unsigned char pitch = 64;
unsigned char mouth = 128;
unsigned char throat = 128;
int singmode = 0;

extern int debug;

unsigned char stress[256];
unsigned char phonemeLength[256];
unsigned char phonemeindex[256];

unsigned char phonemeIndexOutput[60];
unsigned char stressOutput[60];
unsigned char phonemeLengthOutput[60];

int bufferpos=0;
char *buffer = NULL;

void sam_set_buffer(char *buf) { buffer = buf; }

void SetInput(unsigned char *_input)
{
	int i, l;
	l = strlen((char*)_input);
	if (l > 254) l = 254;
	for(i=0; i<l; i++)
		input[i] = _input[i];
	input[l] = 0;
}

void SetSpeed(unsigned char _speed) {speed = _speed;};
void SetPitch(unsigned char _pitch) {pitch = _pitch;};
void SetMouth(unsigned char _mouth) {mouth = _mouth;};
void SetThroat(unsigned char _throat) {throat = _throat;};
void EnableSingmode() {singmode = 1;};
char* GetBuffer(){return buffer;};
int GetBufferLength(){return bufferpos;};

void Init();
int Parser1();
void Parser2();
int SAMMain();
void CopyStress();
void SetPhonemeLength();
void AdjustLengths();
void Code41240();
void Insert(unsigned char position, unsigned char mem60, unsigned char mem59, unsigned char mem58);
void InsertBreath();
void PrepareOutput();
void SetMouthThroat(unsigned char mouth, unsigned char throat);

void Init() {
	int i;
	SetMouthThroat( mouth, throat);

	bufferpos = 0;

	for(i=0; i<256; i++) {
		stress[i] = 0;
		phonemeLength[i] = 0;
	}

	for(i=0; i<60; i++) {
		phonemeIndexOutput[i] = 0;
		stressOutput[i] = 0;
		phonemeLengthOutput[i] = 0;
	}
	phonemeindex[255] = END;
}

int SAMMain() {
	unsigned char X = 0;
	Init();
	phonemeindex[255] = 32;

	if (!Parser1()) return 0;
	if (debug) PrintPhonemes(phonemeindex, phonemeLength, stress);
	Parser2();
	CopyStress();
	SetPhonemeLength();
	AdjustLengths();
	Code41240();
	do {
		if (phonemeindex[X] > 80) {
			phonemeindex[X] = END;
			break;
		}
	} while (++X != 0);
	InsertBreath();

	if (debug) PrintPhonemes(phonemeindex, phonemeLength, stress);

	PrepareOutput();
	return 1;
}

void PrepareOutput() {
	unsigned char srcpos  = 0;
	unsigned char destpos = 0;

	while(1) {
		unsigned char A = phonemeindex[srcpos];
        phonemeIndexOutput[destpos] = A;
        switch(A) {
        case END:
			Render();
			return;
		case BREAK:
			phonemeIndexOutput[destpos] = END;
			Render();
			destpos = 0;
            break;
        case 0:
            break;
        default:
            phonemeLengthOutput[destpos] = phonemeLength[srcpos];
            stressOutput[destpos]        = stress[srcpos];
            ++destpos;
            break;
        }
		++srcpos;
	}
}


void InsertBreath() {
	unsigned char mem54 = 255;
	unsigned char len = 0;
	unsigned char index;

	unsigned char pos = 0;

	while((index = phonemeindex[pos]) != END) {
		len += phonemeLength[pos];
		if (len < 232) {
			if (index == BREAK) {
            } else if (!(flags[index] & FLAG_PUNCT)) {
                if (index == 0) mem54 = pos;
            } else {
                len = 0;
                Insert(++pos, BREAK, 0, 0);
            }
		} else {
            pos = mem54;
            phonemeindex[pos]  = 31;
            phonemeLength[pos] = 4;
            stress[pos] = 0;

            len = 0;
            Insert(++pos, BREAK, 0, 0);
        }
        ++pos;
	}
}


       



void CopyStress() {
	unsigned char pos=0;
    unsigned char Y;
	while((Y = phonemeindex[pos]) != END) {
		if (flags[Y] & 64) {
            Y = phonemeindex[pos+1];

            if (Y != END && (flags[Y] & 128) != 0) {
                Y = stress[pos+1];
                if (Y && !(Y&128)) {
                    stress[pos] = Y+1;
                }
            }
        }

		++pos;
	}
}

void Insert(unsigned char position, unsigned char mem60, unsigned char mem59, unsigned char mem58)
{
	int i;
	for(i=253; i >= position; i--)
	{
		phonemeindex[i+1]  = phonemeindex[i];
		phonemeLength[i+1] = phonemeLength[i];
		stress[i+1]        = stress[i];
	}

	phonemeindex[position]  = mem60;
	phonemeLength[position] = mem59;
	stress[position]        = mem58;
}


signed int full_match(unsigned char sign1, unsigned char sign2) {
    unsigned char Y = 0;
    do {
        unsigned char A = signInputTable1[Y];
        
        if (A == sign1) {
            A = signInputTable2[Y];
            if ((A != '*') && (A == sign2)) return Y;
        }
    } while (++Y != 81);
    return -1;
}

signed int wild_match(unsigned char sign1) {
    signed int Y = 0;
    do {
		if (signInputTable2[Y] == '*') {
			if (signInputTable1[Y] == sign1) return Y;
		}
	} while (++Y != 81);
    return -1;
}




int Parser1()
{
	unsigned char sign1;
	unsigned char position = 0;
	unsigned char srcpos   = 0;

	memset(stress, 0, 256);

	while((sign1 = input[srcpos]) != 155) {
		signed int match;
		unsigned char sign2 = input[++srcpos];
        if ((match = full_match(sign1, sign2)) != -1) {
            phonemeindex[position++] = (unsigned char)match;
            ++srcpos;
        } else if ((match = wild_match(sign1)) != -1) {
            phonemeindex[position++] = (unsigned char)match;
        } else {
            match = 8;
            while( (sign1 != stressInputTable[match]) && (match>0) ) --match;
            
            if (match == 0) return 0;

            stress[position-1] = (unsigned char)match;
        }
	}

    phonemeindex[position] = END;
    return 1;
}


void SetPhonemeLength() {
	int position = 0;
	while(phonemeindex[position] != 255) {
		unsigned char A = stress[position];
		if ((A == 0) || ((A&128) != 0)) {
			phonemeLength[position] = phonemeLengthTable[phonemeindex[position]];
		} else {
			phonemeLength[position] = phonemeStressedLengthTable[phonemeindex[position]];
		}
		position++;
	}
}

void Code41240() {
	unsigned char pos=0;

	while(phonemeindex[pos] != END) {
		unsigned char index = phonemeindex[pos];

		if ((flags[index] & FLAG_STOPCONS)) {
            if ((flags[index] & FLAG_PLOSIVE)) {
                unsigned char A;
                unsigned char X = pos;
                while(!phonemeindex[++X]); 
                A = phonemeindex[X];
                if (A != END) {
                    if ((flags[A] & 8) || (A == 36) || (A == 37)) {++pos; continue;}
                }
                
            }
            Insert(pos+1, index+1, phonemeLengthTable[index+1], stress[pos]);
            Insert(pos+2, index+2, phonemeLengthTable[index+2], stress[pos]);
            pos += 2;
        }
        ++pos;
	}
}


void ChangeRule(unsigned char position, unsigned char mem60, const char * descr)
{
    if (debug) printf("RULE: %s\n",descr);
    phonemeindex[position] = 13;
    Insert(position+1, mem60, 0, stress[position]);
}

void drule(const char * str) {
    if (debug) printf("RULE: %s\n",str);
}

void drule_pre(const char *descr, unsigned char X) {
    drule(descr);
    if (debug) {
        printf("PRE\n");
        printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);
    }
}

void drule_post(unsigned char X) {
    if (debug) {
        printf("POST\n");
        printf("phoneme %d (%c%c) length %d\n", X, signInputTable1[phonemeindex[X]], signInputTable2[phonemeindex[X]], phonemeLength[X]);
    }
}




void rule_alveolar_uw(unsigned char X) {
    if (flags[phonemeindex[X-1]] & FLAG_ALVEOLAR) {
        drule("<ALVEOLAR> UW -> <ALVEOLAR> UX");
        phonemeindex[X] = 16;
    }
}

void rule_ch(unsigned char X) {
    drule("CH -> CH CH+1");
    Insert(X+1, 43, 0, stress[X]);
}

void rule_j(unsigned char X) {
    drule("J -> J J+1");
    Insert(X+1, 45, 0, stress[X]);
}

void rule_g(unsigned char pos) {

    unsigned char index = phonemeindex[pos+1];
            
    if ((index != 255) && ((flags[index] & FLAG_DIP_YX) == 0)) {
        drule("G <VOWEL OR DIPTHONG NOT ENDING WITH IY> -> GX <VOWEL OR DIPTHONG NOT ENDING WITH IY>");
        phonemeindex[pos] = 63;
    }
}


void change(unsigned char pos, unsigned char val, const char * rule) {
    drule(rule);
    phonemeindex[pos] = val;
}


void rule_dipthong(unsigned char p, unsigned short pf, unsigned char pos) {

    unsigned char A = (pf & FLAG_DIP_YX) ? 21 : 20;
                
    if (A==20) drule("insert WX following dipthong NOT ending in IY sound");
    else if (A==21) drule("insert YX following dipthong ending in IY sound");
    Insert(pos+1, A, 0, stress[pos]);
                
    if (p == 53) rule_alveolar_uw(pos);
    else if (p == 42) rule_ch(pos);
    else if (p == 44) rule_j(pos);
}

void Parser2() {
	unsigned char pos = 0;
    unsigned char p;

	if (debug) printf("Parser2\n");

	while((p = phonemeindex[pos]) != END) {
		unsigned short pf;
		unsigned char prior;

		if (debug) printf("%d: %c%c\n", pos, signInputTable1[p], signInputTable2[p]);

		if (p == 0) {
			++pos;
			continue;
		}

        pf = flags[p];
        prior = phonemeindex[pos-1];

        if ((pf & FLAG_DIPTHONG)) rule_dipthong(p, pf, pos);
        else if (p == 78) ChangeRule(pos, 24, "UL -> AX L");
        else if (p == 79) ChangeRule(pos, 27, "UM -> AX M");
        else if (p == 80) ChangeRule(pos, 28, "UN -> AX N");
        else if ((pf & FLAG_VOWEL) && stress[pos]) {
            if (!phonemeindex[pos+1]) {
                p = phonemeindex[pos+2];
                if (p!=END && (flags[p] & FLAG_VOWEL) && stress[pos+2]) {
                    drule("Insert glottal stop between two stressed vowels with space between them");
                    Insert(pos+2, 31, 0, 0);
                }
            }
        } else if (p == pR) {
            if (prior == pT) change(pos-1,42, "T R -> CH R");
            else if (prior == pD) change(pos-1,44, "D R -> J R");
            else if (flags[prior] & FLAG_VOWEL) change(pos, 18, "<VOWEL> R -> <VOWEL> RX");
        } else if (p == 24 && (flags[prior] & FLAG_VOWEL)) change(pos, 19, "<VOWEL> L -> <VOWEL> LX");
        else if (prior == 60 && p == 32) {
            change(pos, 38, "G S -> G Z");
        } else if (p == 60) rule_g(pos);
		else {
            if (p == 72) {
                unsigned char Y = phonemeindex[pos+1];
                if ((flags[Y] & FLAG_DIP_YX)==0 || Y==END) {
                    change(pos, 75, "K <VOWEL OR DIPTHONG NOT ENDING WITH IY> -> KX <VOWEL OR DIPTHONG NOT ENDING WITH IY>");
                    p  = 75;
                    pf = flags[p];
                }
            }

            if ((flags[p] & FLAG_PLOSIVE) && (prior == 32)) {
                
                if (debug) printf("RULE: S* %c%c -> S* %c%c\n", signInputTable1[p], signInputTable2[p],signInputTable1[p-12], signInputTable2[p-12]);
                phonemeindex[pos] = p-12;
            } else if (!(pf & FLAG_PLOSIVE)) {
                p = phonemeindex[pos];
                if (p == 53) rule_alveolar_uw(pos);
                else if (p == 42) rule_ch(pos);
                else if (p == 44) rule_j(pos);
            }
            
            if (p == 69 || p == 57) {
                if (flags[phonemeindex[pos-1]] & FLAG_VOWEL) {
                    p = phonemeindex[pos+1];
                    if (!p) p = phonemeindex[pos+2];
                    if ((flags[p] & FLAG_VOWEL) && !stress[pos+1]) change(pos,30, "Soften T or D following vowel or ER and preceding a pause -> DX");
                }
            }
        }
        pos++;
	}
}

void AdjustLengths() {

	{
	unsigned char X = 0;
	unsigned char index;

	while((index = phonemeindex[X]) != END) {
		unsigned char loopIndex;

		if((flags[index] & FLAG_PUNCT) == 0) {
			++X;
			continue;
		}

		loopIndex = X;

        while (--X && !(flags[phonemeindex[X]] & FLAG_VOWEL));
        if (X == 0) break;

		do {
			index = phonemeindex[X];

			if(!(flags[index] & FLAG_FRICATIVE) || (flags[index] & FLAG_VOICED)) {
				unsigned char A = phonemeLength[X];
                drule_pre("Lengthen <FRICATIVE> or <VOICED> between <VOWEL> and <PUNCTUATION> by 1.5",X);
				phonemeLength[X] = (A >> 1) + A + 1;
                drule_post(X);
			}
		} while (++X != loopIndex);
		X++;
	}
	}


	unsigned char loopIndex=0;
	unsigned char index;

	while((index = phonemeindex[loopIndex]) != END) {
		unsigned char X = loopIndex;

		if (flags[index] & FLAG_VOWEL) {
			index = phonemeindex[loopIndex+1];
			if (!(flags[index] & FLAG_CONSONANT)) {
				if ((index == 18) || (index == 19)) {
					index = phonemeindex[loopIndex+2];
					if ((flags[index] & FLAG_CONSONANT)) {
                        drule_pre("<VOWEL> <RX | LX> <CONSONANT> - decrease length of vowel by 1\n", loopIndex);
    					phonemeLength[loopIndex]--;
                        drule_post(loopIndex);
                    }
				}
			} else {
                unsigned short flag = (index == END) ? 65 : flags[index];

                if (!(flag & FLAG_VOICED)) {
                    if((flag & FLAG_PLOSIVE)) {
                        drule_pre("<VOWEL> <UNVOICED PLOSIVE> - decrease vowel by 1/8th",loopIndex);
                        phonemeLength[loopIndex] -= (phonemeLength[loopIndex] >> 3);
                        drule_post(loopIndex);
                    }
                } else {
                    unsigned char A;
                    drule_pre("<VOWEL> <VOICED CONSONANT> - increase vowel by 1/2 + 1\n",X-1);
                    A = phonemeLength[loopIndex];
                    phonemeLength[loopIndex] = (A >> 2) + A + 1;
                    drule_post(loopIndex);
                }
            }
		} else if((flags[index] & FLAG_NASAL) != 0) {
            index = phonemeindex[++X];
            if (index != END && (flags[index] & FLAG_STOPCONS)) {
                drule("<NASAL> <STOP CONSONANT> - set nasal = 5, consonant = 6");
                phonemeLength[X]   = 6;
                phonemeLength[X-1] = 5;
            }
        } else if((flags[index] & FLAG_STOPCONS)) {

            while ((index = phonemeindex[++X]) == 0);

            if (index != END && (flags[index] & FLAG_STOPCONS)) {
                drule("<UNVOICED STOP CONSONANT> {optional silence} <STOP CONSONANT> - shorten both to 1/2 + 1");
                phonemeLength[X]         = (phonemeLength[X] >> 1) + 1;
                phonemeLength[loopIndex] = (phonemeLength[loopIndex] >> 1) + 1;
                X = loopIndex;
            }
        } else if ((flags[index] & FLAG_LIQUIC)) {
            index = phonemeindex[X-1];

            if((flags[index] & FLAG_STOPCONS) != 0) 
                drule_pre("<LIQUID CONSONANT> <DIPTHONG> - decrease by 2",X);
            
            phonemeLength[X] -= 2;
            drule_post(X);
         }

        ++loopIndex;
    }
}
