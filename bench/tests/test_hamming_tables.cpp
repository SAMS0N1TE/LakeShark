#include "Hamming.hpp"
#include <cstdio>

int main()
{
    Hamming_10_6_3 reference;
    Hamming_10_6_3_TableImpl table;
    for (int input = 0; input < 1024; ++input) {
        int expected, actual;
        int errors = reference.decode(input, &expected);
        if (table.decode(input, &actual) != errors || actual != expected) {
            std::fprintf(stderr, "decode mismatch at %d\n", input); return 1;
        }
    }
    for (int input = 0; input < 64; ++input)
        if (table.encode(input) != reference.encode(input)) return 1;
    std::puts("1024 decode entries and 64 encode entries match");
    return 0;
}
