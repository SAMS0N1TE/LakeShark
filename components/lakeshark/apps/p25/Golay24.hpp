
#ifndef GOLAY24_HPP_7a68240afda9406facf81fcad3851111
#define GOLAY24_HPP_7a68240afda9406facf81fcad3851111

#include <stdio.h>
#include <assert.h>

#define POLY  0xAE3

class Golay24
{
private:
    unsigned int golay(unsigned int cw)

    {
      int i;
      unsigned int c;
      cw&=0xfffl;
      c=cw;
      for (i=1; i<=12; i++)
        {
          if (cw & 1)
            cw^=POLY;
          cw>>=1;
        }
      return((cw<<12)|c);
    }

    int parity(unsigned int cw)

    {
      unsigned char p;

      p=*(unsigned char*)&cw;
      p^=*((unsigned char*)&cw+1);
      p^=*((unsigned char*)&cw+2);

      p=p ^ (p>>4);
      p=p ^ (p>>2);
      p=p ^ (p>>1);

      return(p & 1);
    }

    unsigned int syndrome(unsigned int cw)

    {
      int i;
      cw&=0x7fffffl;
      for (i=1; i<=12; i++)
        {
          if (cw & 1)
            cw^=POLY;
          cw>>=1;
        }
      return(cw<<12);
    }

    int weight(unsigned int cw)

    {
      int bits,k;

      const char wgt[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};

      bits=0;
      k=0;

      while ((k<6) && (cw))
        {
          bits=bits+wgt[cw & 0xf];
          cw>>=4;
          k++;
        }

      return(bits);
    }

    unsigned int rotate_left(unsigned int cw, int n)

    {
      int i;

      if (n != 0)
        {
          for (i=1; i<=n; i++)
            {
              if ((cw & 0x400000l) != 0)
                cw=(cw << 1) | 1;
              else
                cw<<=1;
            }
        }

      return(cw & 0x7fffffl);
    }

    unsigned int rotate_right(unsigned int cw, int n)

    {
      int i;

      if (n != 0)
        {
          for (i=1; i<=n; i++)
            {
              if ((cw & 1) != 0)
                cw=(cw >> 1) | 0x400000l;
              else
                cw>>=1;
            }
        }

      return(cw & 0x7fffffl);
    }

public:
    unsigned int correct(unsigned int cw, int *errs, unsigned int *errors_detected)

    {
      unsigned char
        w;
      unsigned int
        mask;
      int
        i,j;
      unsigned int
        s,
        cwsaver;

      cwsaver=cw;
      *errs=0;
      *errors_detected = 0;

      w=3;
      j=-1;
      mask=1;
      while (j<23)
        {
          if (j != -1)
            {
              if (j>0)
                {
                  cw=cwsaver ^ mask;
                  mask+=mask;
                }
              cw=cwsaver ^ mask;
              w=2;
            }

          s=syndrome(cw);
          if (s)
            {
              (*errors_detected)++;
              for (i=0; i<23; i++)
                {
                  if ((*errs=weight(s)) <= w)
                    {
                      cw=cw ^ s;
                      cw=rotate_right(cw,i);
                      return(s=cw);
                    }
                  else
                    {
                      cw=rotate_left(cw,1);
                      s=syndrome(cw);
                    }
                }
              j++;
            }
          else
            {
              return(cw);
            }
        }

      return(cwsaver);
    }

    unsigned int encode(unsigned int data)
    {
        unsigned int codeword =golay(data);
        if (parity(codeword)) {
          codeword^=0x800000l;
        }

        return codeword;
    }

    unsigned int encode23(unsigned int data)
    {
        return golay(data);
    }

    int decode(int *errs, unsigned int *cw)

    {
      unsigned int parity_bit;
      unsigned int detected_errors;
      parity_bit=*cw & 0x800000l;
      *cw&=~0x800000l;

      *cw=correct(*cw, errs, &detected_errors);
      *cw|=parity_bit;

      if (parity(*cw))
        return(1);

      return(0);
    }

    int detect(int *errs, unsigned int cw)

    {
      *errs=0;
      if (parity(cw))
        {
          *errs=1;
          return(1);
        }
      if (syndrome(cw))
        {
          *errs=1;
          return(2);
        }
      else
        return(0);

    }
};

class DSDGolay24 : public Golay24
{
public:
    unsigned int adapt_to_codeword(const char* word, unsigned int length, const char* parity)
    {
        unsigned int codeword = 0;

        for (unsigned int i=0; i<12; i++) {
            assert(parity[11-i] == 0 || parity[11-i] == 1);
            codeword <<= 1;
            codeword |= parity[11-i];
        }
        for (unsigned int i=0; i<length; i++) {
            char bit = word[length-1-i];
            assert(bit == 0 || bit == 1);
            codeword <<= 1;
            codeword |= bit;
        }

        if (length < 12) {
            codeword <<= (12 - length);
        }

        return codeword;
    }

    void adapt_to_word(unsigned int codeword, char* word, unsigned int length)
    {

        for (unsigned int i=0, mask = 1<<(12-length); i<length; i++, mask<<=1) {
            word[i] = (codeword & mask) != 0? 1 : 0;
        }
    }

    unsigned int adapt_from_word(char* word, unsigned int length)
    {
        unsigned int codeword = 0;

        for (unsigned int i=0; i<length; i++) {
            int bit = word[length-1-i];
            assert(bit == 0 || bit == 1);
            codeword <<= 1;
            codeword |= bit;
        }

        if (length < 12) {
            codeword <<= (12-length);
        }

        return codeword;
    }

    int decode_6(char* hex, const char* parity, int* fixed_errors)
    {
        unsigned int codeword = adapt_to_codeword(hex, 6, parity);

        int uncorrectable_errors = Golay24::decode(fixed_errors, &codeword);

        if (uncorrectable_errors == 1 && (codeword & 0x3f) != 0) {

        } else {

            adapt_to_word(codeword, hex, 6);
            uncorrectable_errors = 0;
        }

        return uncorrectable_errors;
    }

    int decode_12(char* dodeca, const char* parity, int* fixed_errors)
    {
        unsigned int codeword = adapt_to_codeword(dodeca, 12, parity);

        int uncorrectable_errors = Golay24::decode(fixed_errors, &codeword);

        if (uncorrectable_errors == 1 && (codeword & 0x3f) != 0) {

        } else {

            adapt_to_word(codeword, dodeca, 12);
            uncorrectable_errors = 0;
        }

        return uncorrectable_errors;
    }

    void encode_6(char* hex, char* out_parity)
    {
        unsigned int data = adapt_from_word(hex, 6);
        unsigned int codeword = Golay24::encode(data);

        for (unsigned int i=0, mask = 1<<12; i<12; i++, mask<<=1) {
            out_parity[i] = (codeword & mask) != 0? 1 : 0;
        }
    }

    void encode_12(char* dodeca, char* out_parity)
    {
        unsigned int data = adapt_from_word(dodeca, 12);
        unsigned int codeword = Golay24::encode(data);

        for (unsigned int i=0, mask = 1<<12; i<12; i++, mask<<=1) {
            out_parity[i] = (codeword & mask) != 0? 1 : 0;
        }
    }
};

#endif
