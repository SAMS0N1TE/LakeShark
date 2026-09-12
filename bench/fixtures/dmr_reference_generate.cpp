#include "BPTC19696.h"
#include "Golay2087.h"
#include "RS129.h"
#include "Utils.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
void CUtils::byteToBitsBE(unsigned char c,bool *b){for(int i=0;i<8;i++)b[i]=(c>>(7-i))&1;}
void CUtils::bitsToByteBE(const bool *b,unsigned char &c){c=0;for(int i=0;i<8;i++)c=(c<<1)|b[i];}
static void dump(const unsigned char *b,int n){for(int i=0;i<n;i++)printf("%02x",b[i]);puts("");}
int main(){
 for(int i=0;i<256;i++){unsigned char b[3]={(unsigned char)i,0,0};CGolay2087::encode(b);dump(b,3);}
 unsigned char messages[][12]={{0,0,0,0,0,91,0x12,0x34,0x56},{0x83,0x10,0xC0,0xFE,0xDC,0xBA,0x65,0x43,0x21},{0xff,0x55,0xaa,0x12,0x34,0x56,0x78,0x9a,0xbc}};
 for(auto &m:messages){unsigned char parity[4];CRS129::encode(m,9,parity);for(int mask:{0x96,0x99}){for(int i=0;i<3;i++)m[9+i]=parity[2-i]^mask;unsigned char b[33]={0};CBPTC19696 encoder;encoder.encode(m,b);dump(m,12);dump(b,33);}}
}
