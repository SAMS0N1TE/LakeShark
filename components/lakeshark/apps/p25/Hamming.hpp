
#ifndef HAMMING_HPP_e7123c3795b94d14b5774d5d8f016a04
#define HAMMING_HPP_e7123c3795b94d14b5774d5d8f016a04

#include <bitset>
#include <string>
#include <assert.h>

#include <iostream>

class Hamming_10_6_3_data
{
public:
    std::bitset<10> g0;
    std::bitset<10> g1;
    std::bitset<10> g2;
    std::bitset<10> g3;
    std::bitset<10> g4;
    std::bitset<10> g5;

    std::bitset<6> gt0;
    std::bitset<6> gt1;
    std::bitset<6> gt2;
    std::bitset<6> gt3;

    std::bitset<10> h0;
    std::bitset<10> h1;
    std::bitset<10> h2;
    std::bitset<10> h3;

    int bad_bit_table[16];

    Hamming_10_6_3_data()
    {

        g0 = std::bitset<10>(std::string("1000001110"));
        g1 = std::bitset<10>(std::string("0100001101"));
        g2 = std::bitset<10>(std::string("0010001011"));
        g3 = std::bitset<10>(std::string("0001000111"));
        g4 = std::bitset<10>(std::string("0000100011"));
        g5 = std::bitset<10>(std::string("0000011100"));

        gt0 = std::bitset<6>(std::string("111001"));
        gt1 = std::bitset<6>(std::string("110101"));
        gt2 = std::bitset<6>(std::string("101110"));
        gt3 = std::bitset<6>(std::string("011110"));

        h0 = std::bitset<10>(std::string("1110011000"));
        h1 = std::bitset<10>(std::string("1101010100"));
        h2 = std::bitset<10>(std::string("1011100010"));
        h3 = std::bitset<10>(std::string("0111100001"));

        bad_bit_table[ 1] = 0;
        bad_bit_table[ 2] = 1;
        bad_bit_table[ 4] = 2;
        bad_bit_table[ 8] = 3;
        bad_bit_table[12] = 4;
        bad_bit_table[ 3] = 5;
        bad_bit_table[ 7] = 6;
        bad_bit_table[11] = 7;
        bad_bit_table[13] = 8;
        bad_bit_table[14] = 9;

        bad_bit_table[ 0] = -2;

        bad_bit_table[ 5] = -1;
        bad_bit_table[ 6] = -1;
        bad_bit_table[ 9] = -1;
        bad_bit_table[10] = -1;
        bad_bit_table[15] = -1;
    }
};

class Hamming_15_11_3_data
{
public:
    std::bitset<15> g0;
    std::bitset<15> g1;
    std::bitset<15> g2;
    std::bitset<15> g3;
    std::bitset<15> g4;
    std::bitset<15> g5;
    std::bitset<15> g6;
    std::bitset<15> g7;
    std::bitset<15> g8;
    std::bitset<15> g9;
    std::bitset<15> g10;

    std::bitset<11> gt0;
    std::bitset<11> gt1;
    std::bitset<11> gt2;
    std::bitset<11> gt3;

    std::bitset<15> h0;
    std::bitset<15> h1;
    std::bitset<15> h2;
    std::bitset<15> h3;

    int bad_bit_table[16];

    Hamming_15_11_3_data()
    {

        g0  = std::bitset<15>(std::string("100000000001111"));
        g1  = std::bitset<15>(std::string("010000000001110"));
        g2  = std::bitset<15>(std::string("001000000001101"));
        g3  = std::bitset<15>(std::string("000100000001100"));
        g4  = std::bitset<15>(std::string("000010000001011"));
        g5  = std::bitset<15>(std::string("000001000001010"));
        g6  = std::bitset<15>(std::string("000000100001001"));
        g7  = std::bitset<15>(std::string("000000010000111"));
        g8  = std::bitset<15>(std::string("000000001000110"));
        g9  = std::bitset<15>(std::string("000000000100101"));
        g10 = std::bitset<15>(std::string("000000000010011"));

        gt0 = std::bitset<11>(std::string("11111110000"));
        gt1 = std::bitset<11>(std::string("11110001110"));
        gt2 = std::bitset<11>(std::string("11001101101"));
        gt3 = std::bitset<11>(std::string("10101011011"));

        h0 = std::bitset<15>(std::string("111111100001000"));
        h1 = std::bitset<15>(std::string("111100011100100"));
        h2 = std::bitset<15>(std::string("110011011010010"));
        h3 = std::bitset<15>(std::string("101010110110001"));

        bad_bit_table[ 1] = 0;
        bad_bit_table[ 2] = 1;
        bad_bit_table[ 4] = 2;
        bad_bit_table[ 8] = 3;
        bad_bit_table[12] = 4;
        bad_bit_table[ 3] = 5;
        bad_bit_table[ 7] = 6;
        bad_bit_table[11] = 7;
        bad_bit_table[13] = 8;
        bad_bit_table[14] = 9;

        bad_bit_table[ 0] = -2;

        bad_bit_table[ 5] = -1;
        bad_bit_table[ 6] = -1;
        bad_bit_table[ 9] = -1;
        bad_bit_table[10] = -1;
        bad_bit_table[15] = -1;
    }
};

class Hamming_Inteface
{
public:
    virtual ~Hamming_Inteface()
    {

    }

    virtual int decode(int input, int* output) = 0;

    virtual int decode(char* hex, char* parity) = 0;

    virtual int encode(int input) = 0;

    virtual void encode(char* hex, char* out_parity) = 0;
};

class Hamming_10_6_3 : public Hamming_Inteface
{
private:
    static Hamming_10_6_3_data data;

public:

    int decode(std::bitset<10>& input);

    int decode(int input, int* output)
    {
        assert (input < 1024 && input >= 0);

        std::bitset<10> bitset_input(input);
        int error_count = decode(bitset_input);
        if (error_count == 1) {

            input = (int)bitset_input.to_ulong();
        } else {

        }

        *output = input >> 4;

        return error_count;
    }

    int decode(char* hex, char* parity)
    {

        std::bitset<10> value;

        for (unsigned int i=0; i<6; i++) {
            value[9-i] = (hex[i] == 1)? true : false;
        }
        for (unsigned int i=0; i<4; i++) {
            value[3-i] = (parity[i] == 1)? true : false;
        }

        int error_count = decode(value);

        if (error_count == 1) {
            for (unsigned int i=0; i<6; i++) {
                hex[i] = value[9-i];
            }
        } else {

        }

        return error_count;
    }

    int encode(std::bitset<6>& input);

    int encode(int input)
    {
        assert (input < 64 && input >= 0);

        std::bitset<6> bitset_input(input);

        return encode(bitset_input);
    }

    void encode(char* hex, char* out_parity)
    {

        std::bitset<6> value;

        for (unsigned int i=0; i<6; i++) {
            value[5-i] = (hex[i] == 1)? true : false;
        }

        int parity = encode(value);

        for (int i=3; i>=0; i--) {
            out_parity[i] = parity & 1;
            parity >>= 1;
        }
    }
};

class Hamming_10_6_3_TableImpl_data
{
public:
    int fixed_values[1024];
    int error_counts[1024];
    int encode_parities[64];

    Hamming_10_6_3_TableImpl_data()
    {
        Hamming_10_6_3 hamming;

        for (int i=0; i<1024; i++) {
            int fixed;
            int error_count = hamming.decode(i, &fixed);
            fixed_values[i] = fixed;
            error_counts[i] = error_count;
        }

        for (int i=0; i<64; i++) {
            int parity = hamming.encode(i);
            encode_parities[i] = parity;
        }
    }
};

class Hamming_10_6_3_TableImpl : public Hamming_Inteface
{
private:
    static Hamming_10_6_3_TableImpl_data data;

public:
    int decode(int input, int* output);

    static int convert_hex_to_int(char* hex)
    {

        int value = 0;

        for (unsigned int i=0; i<6; i++) {
            assert(hex[i] == 0 || hex[i] == 1);
            value <<= 1;
            value |= hex[i];
        }

        return value;
    }

    static int convert_hex_parity_to_int(char* hex, char* parity)
    {

        int value = 0;

        for (unsigned int i=0; i<6; i++) {
            assert(hex[i] == 0 || hex[i] == 1);
            value <<= 1;
            value |= hex[i];
        }
        for (unsigned int i=0; i<4; i++) {
            assert(parity[i] == 0 || parity[i] == 1);
            value <<= 1;
            value |= parity[i];
        }

        return value;
    }

    static void convert_int_to_hex(int value, char* hex)
    {
        unsigned int v = value;
        for (unsigned int i=0; i<6; i++) {
            hex[5-i] = v & 1;
            v >>= 1;
        }
    }

    int decode(char* hex, char* parity)
    {
        int value = convert_hex_parity_to_int(hex, parity);
        int fixed;
        int error_count = decode(value, &fixed);

        if (error_count == 1) {
            convert_int_to_hex(fixed, hex);
        } else {

        }

        return error_count;
    }

    int encode(int input);

    void encode(char* hex, char* out_parity)
    {
        int value = convert_hex_to_int(hex);
        int parity = encode(value);

        for (int i=3; i>=0; i--) {
            out_parity[i] = parity & 1;
            parity >>= 1;
        }
    }
};

#endif
