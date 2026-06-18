#ifndef SAM_H
#define SAM_H

#ifdef __cplusplus
extern "C" {
#endif

void SetInput(unsigned char *_input);
void SetSpeed(unsigned char _speed);
void SetPitch(unsigned char _pitch);
void SetMouth(unsigned char _mouth);
void SetThroat(unsigned char _throat);
void EnableSingmode();

int SAMMain();

char* GetBuffer();
int GetBufferLength();

void sam_set_buffer(char *buf);

#ifdef __cplusplus
}
#endif

#endif
