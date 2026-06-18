
#ifndef REEDSOLOMON_HPP_b1405fdab6374ba2a4e65e8d45ec3d80
#define REEDSOLOMON_HPP_b1405fdab6374ba2a4e65e8d45ec3d80

#include <math.h>
#include <stdio.h>

template <int TT>
class ReedSolomon_63
{
private:
    static const int MM = 6;
    static const int NN = 63;

    static const int KK = NN-2*TT;

    int* alpha_to;
    int* index_of;
    int* gg;

    void generate_gf(int* generator_polinomial)

    {
        register int i, mask;

        mask = 1;
        alpha_to[MM] = 0;
        for (i = 0; i < MM; i++) {
            alpha_to[i] = mask;
            index_of[alpha_to[i]] = i;
            if (generator_polinomial[i] != 0)
                alpha_to[MM] ^= mask;
            mask <<= 1;
        }
        index_of[alpha_to[MM]] = MM;
        mask >>= 1;
        for (i = MM + 1; i < NN; i++) {
            if (alpha_to[i - 1] >= mask)
                alpha_to[i] = alpha_to[MM] ^ ((alpha_to[i - 1] ^ mask) << 1);
            else
                alpha_to[i] = alpha_to[i - 1] << 1;
            index_of[alpha_to[i]] = i;
        }
        index_of[0] = -1;
    }

    void gen_poly()

    {
        register int i, j;

        gg[0] = 2;
        gg[1] = 1;
        for (i = 2; i <= NN - KK; i++) {
            gg[i] = 1;
            for (j = i - 1; j > 0; j--)
                if (gg[j] != 0)
                    gg[j] = gg[j - 1] ^ alpha_to[(index_of[gg[j]] + i) % NN];
                else
                    gg[j] = gg[j - 1];
            gg[0] = alpha_to[(index_of[gg[0]] + i) % NN];
        }

        for (i = 0; i <= NN - KK; i++)
            gg[i] = index_of[gg[i]];
    }

public:
    ReedSolomon_63()
    {
        alpha_to = new int[NN + 1];
        index_of = new int[NN + 1];
        gg       = new int[NN - KK + 1];

        int generator_polinomial[] = { 1, 1, 0, 0, 0, 0, 1 };

        generate_gf(generator_polinomial);

        gen_poly();
    }

    virtual ~ReedSolomon_63()
    {
        delete[] gg;
        delete[] index_of;
        delete[] alpha_to;
    }

    void encode(const int* data, int* bb)

    {
        register int i, j;
        int feedback;

        for (i = 0; i < NN - KK; i++)
            bb[i] = 0;
        for (i = KK - 1; i >= 0; i--) {
            feedback = index_of[data[i] ^ bb[NN - KK - 1]];
            if (feedback != -1) {
                for (j = NN - KK - 1; j > 0; j--)
                    if (gg[j] != -1)
                        bb[j] = bb[j - 1] ^ alpha_to[(gg[j] + feedback) % NN];
                    else
                        bb[j] = bb[j - 1];
                bb[0] = alpha_to[(gg[0] + feedback) % NN];
            } else {
                for (j = NN - KK - 1; j > 0; j--)
                    bb[j] = bb[j - 1];
                bb[0] = 0;
            }
        }
    }

    int decode(const int* input, int* recd)

    {
        register int i, j, u, q;
        int elp[NN - KK + 2][NN - KK], d[NN - KK + 2], l[NN - KK + 2], u_lu[NN - KK
                + 2], s[NN - KK + 1];
        int count = 0, syn_error = 0, root[TT], loc[TT], z[TT + 1], err[NN], reg[TT
                + 1];

        int irrecoverable_error = 0;

        for (int i = 0; i < NN; i++)
            recd[i] = index_of[input[i]];

        for (i = 1; i <= NN - KK; i++) {
            s[i] = 0;
            for (j = 0; j < NN; j++)
                if (recd[j] != -1)
                    s[i] ^= alpha_to[(recd[j] + i * j) % NN];

            if (s[i] != 0)
                syn_error = 1;
            s[i] = index_of[s[i]];
        }

        if (syn_error)
        {

            d[0] = 0;
            d[1] = s[1];
            elp[0][0] = 0;
            elp[1][0] = 1;
            for (i = 1; i < NN - KK; i++) {
                elp[0][i] = -1;
                elp[1][i] = 0;
            }
            l[0] = 0;
            l[1] = 0;
            u_lu[0] = -1;
            u_lu[1] = 0;
            u = 0;

            do {
                u++;
                if (d[u] == -1) {
                    l[u + 1] = l[u];
                    for (i = 0; i <= l[u]; i++) {
                        elp[u + 1][i] = elp[u][i];
                        elp[u][i] = index_of[elp[u][i]];
                    }
                } else

                {
                    q = u - 1;
                    while ((d[q] == -1) && (q > 0))
                        q--;

                    if (q > 0) {
                        j = q;
                        do {
                            j--;
                            if ((d[j] != -1) && (u_lu[q] < u_lu[j]))
                                q = j;
                        } while (j > 0);
                    };

                    if (l[u] > l[q] + u - q)
                        l[u + 1] = l[u];
                    else
                        l[u + 1] = l[q] + u - q;

                    for (i = 0; i < NN - KK; i++)
                        elp[u + 1][i] = 0;
                    for (i = 0; i <= l[q]; i++)
                        if (elp[q][i] != -1)
                            elp[u + 1][i + u - q] = alpha_to[(d[u] + NN - d[q]
                                    + elp[q][i]) % NN];
                    for (i = 0; i <= l[u]; i++) {
                        elp[u + 1][i] ^= elp[u][i];
                        elp[u][i] = index_of[elp[u][i]];
                    }
                }
                u_lu[u + 1] = u - l[u + 1];

                if (u < NN - KK)
                {
                    if (s[u + 1] != -1)
                        d[u + 1] = alpha_to[s[u + 1]];
                    else
                        d[u + 1] = 0;
                    for (i = 1; i <= l[u + 1]; i++)
                        if ((s[u + 1 - i] != -1) && (elp[u + 1][i] != 0))
                            d[u + 1] ^= alpha_to[(s[u + 1 - i]
                                    + index_of[elp[u + 1][i]]) % NN];
                    d[u + 1] = index_of[d[u + 1]];
                }
            } while ((u < NN - KK) && (l[u + 1] <= TT));

            u++;
            if (l[u] <= TT)
            {

                for (i = 0; i <= l[u]; i++)
                    elp[u][i] = index_of[elp[u][i]];

                for (i = 1; i <= l[u]; i++)
                    reg[i] = elp[u][i];
                count = 0;
                for (i = 1; i <= NN; i++) {
                    q = 1;
                    for (j = 1; j <= l[u]; j++)
                        if (reg[j] != -1) {
                            reg[j] = (reg[j] + j) % NN;
                            q ^= alpha_to[reg[j]];
                        };
                    if (!q)
                    {
                        root[count] = i;
                        loc[count] = NN - i;
                        count++;
                    };
                };

                if (count == l[u])
                {

                    for (i = 1; i <= l[u]; i++)
                    {
                        if ((s[i] != -1) && (elp[u][i] != -1))
                            z[i] = alpha_to[s[i]] ^ alpha_to[elp[u][i]];
                        else if ((s[i] != -1) && (elp[u][i] == -1))
                            z[i] = alpha_to[s[i]];
                        else if ((s[i] == -1) && (elp[u][i] != -1))
                            z[i] = alpha_to[elp[u][i]];
                        else
                            z[i] = 0;
                        for (j = 1; j < i; j++)
                            if ((s[j] != -1) && (elp[u][i - j] != -1))
                                z[i] ^= alpha_to[(elp[u][i - j] + s[j]) % NN];
                        z[i] = index_of[z[i]];
                    };

                    for (i = 0; i < NN; i++) {
                        err[i] = 0;
                        if (recd[i] != -1)
                            recd[i] = alpha_to[recd[i]];
                        else
                            recd[i] = 0;
                    }
                    for (i = 0; i < l[u]; i++)
                    {
                        err[loc[i]] = 1;
                        for (j = 1; j <= l[u]; j++)
                            if (z[j] != -1)
                                err[loc[i]] ^= alpha_to[(z[j] + j * root[i]) % NN];
                        if (err[loc[i]] != 0) {
                            err[loc[i]] = index_of[err[loc[i]]];
                            q = 0;
                            for (j = 0; j < l[u]; j++)
                                if (j != i)
                                    q += index_of[1
                                            ^ alpha_to[(loc[j] + root[i]) % NN]];
                            q = q % NN;
                            err[loc[i]] = alpha_to[(err[loc[i]] - q + NN) % NN];
                            recd[loc[i]] ^= err[loc[i]];
                        }
                    }
                } else {

                    irrecoverable_error = 1;
                }

            } else {

                irrecoverable_error = 1;
            }

        } else {

            for (i = 0; i < NN; i++)
                if (recd[i] != -1)
                    recd[i] = alpha_to[recd[i]];
                else
                    recd[i] = 0;
        }

        if (irrecoverable_error) {
            for (i = 0; i < NN; i++)
                if (recd[i] != -1)
                    recd[i] = alpha_to[recd[i]];
                else
                    recd[i] = 0;
        }

        return irrecoverable_error;
    }

protected:
    int bin_to_hex(const char* input)
    {
        int output = ((input[0] != 0)? 32 : 0) |
                     ((input[1] != 0)? 16 : 0) |
                     ((input[2] != 0)?  8 : 0) |
                     ((input[3] != 0)?  4 : 0) |
                     ((input[4] != 0)?  2 : 0) |
                     ((input[5] != 0)?  1 : 0);

        return output;
    }

    void hex_to_bin(int input, char* output)
    {
        output[0] = ((input & 32) != 0)?  1 : 0;
        output[1] = ((input & 16) != 0)?  1 : 0;
        output[2] = ((input &  8) != 0)?  1 : 0;
        output[3] = ((input &  4) != 0)?  1 : 0;
        output[4] = ((input &  2) != 0)?  1 : 0;
        output[5] = ((input &  1) != 0)?  1 : 0;
    }
};

class DSDReedSolomon_36_20_17 : public ReedSolomon_63<8>
{
public:

    DSDReedSolomon_36_20_17() : ReedSolomon_63<8>()
    {

    }

    int decode(char* hex_data, const char* hex_parity)
    {
        int input[63];
        int output[63];

        for(unsigned int i=0; i<16; i++) {
            input[i] = bin_to_hex(hex_parity + i*6);
        }

        for(unsigned int i=16; i<16+20; i++) {
            input[i] = bin_to_hex(hex_data + (i-16)*6);
        }

        for(unsigned int i=16+20; i<63; i++) {
            input[i] = 0;
        }

        int irrecoverable_errors = ReedSolomon_63<8>::decode(input, output);

        for(unsigned int i=16; i<16+20; i++) {
            hex_to_bin(output[i], hex_data + (i-16)*6);
        }

        return irrecoverable_errors;
    }

    void encode(const char* hex_data, char* out_hex_parity)
    {
        int input[47];
        int output[63];

        for(unsigned int i=0; i<20; i++) {
            input[i] = bin_to_hex(hex_data + i*6);
        }

        for(unsigned int i=20; i<47; i++) {
            input[i] = 0;
        }

        ReedSolomon_63<8>::encode(input, output);

        for(unsigned int i=0; i<16; i++) {
            hex_to_bin(output[i], out_hex_parity + i*6);
        }
    }
};

class DSDReedSolomon_24_12_13 : public ReedSolomon_63<6>
{
public:

    DSDReedSolomon_24_12_13() : ReedSolomon_63<6>()
    {

    }

    int decode(char* hex_data, const char* hex_parity)
    {
        int input[63];
        int output[63];

        for(unsigned int i=0; i<12; i++) {
            input[i] = bin_to_hex(hex_parity + i*6);
        }

        for(unsigned int i=12; i<12+12; i++) {
            input[i] = bin_to_hex(hex_data + (i-12)*6);
        }

        for(unsigned int i=12+12; i<63; i++) {
            input[i] = 0;
        }

        int irrecoverable_errors = ReedSolomon_63<6>::decode(input, output);

        for(unsigned int i=12; i<12+12; i++) {
            hex_to_bin(output[i], hex_data + (i-12)*6);
        }

        return irrecoverable_errors;
    }

    void encode(const char* hex_data, char* out_hex_parity)
    {
        int input[51];
        int output[63];

        for(unsigned int i=0; i<12; i++) {
            input[i] = bin_to_hex(hex_data + i*6);
        }

        for(unsigned int i=12; i<51; i++) {
            input[i] = 0;
        }

        ReedSolomon_63<6>::encode(input, output);

        for(unsigned int i=0; i<12; i++) {
            hex_to_bin(output[i], out_hex_parity + i*6);
        }
    }

};

class DSDReedSolomon_24_16_9 : public ReedSolomon_63<4>
{
public:

    DSDReedSolomon_24_16_9() : ReedSolomon_63<4>()
    {

    }

    int decode(char* hex_data, const char* hex_parity)
    {
        int input[63];
        int output[63];

        for(unsigned int i=0; i<8; i++) {
            input[i] = bin_to_hex(hex_parity + i*6);
        }

        for(unsigned int i=8; i<8+16; i++) {
            input[i] = bin_to_hex(hex_data + (i-8)*6);
        }

        for(unsigned int i=8+16; i<63; i++) {
            input[i] = 0;
        }

        int irrecoverable_errors = ReedSolomon_63<4>::decode(input, output);

        for(unsigned int i=8; i<8+16; i++) {
            hex_to_bin(output[i], hex_data + (i-8)*6);
        }

        return irrecoverable_errors;
    }

    void encode(const char* hex_data, char* out_hex_parity)
    {
        int input[55];
        int output[63];

        for(unsigned int i=0; i<16; i++) {
            input[i] = bin_to_hex(hex_data + i*6);
        }

        for(unsigned int i=16; i<55; i++) {
            input[i] = 0;
        }

        ReedSolomon_63<4>::encode(input, output);

        for(unsigned int i=0; i<8; i++) {
            hex_to_bin(output[i], out_hex_parity + i*6);
        }
    }
};

#endif
