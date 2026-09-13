
// P25 TDMA Decoder (C) Copyright 2013, 2014 Max H. Parke KA1RBI
// 
// This file is part of OP25
// 
// OP25 is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
// 
// OP25 is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
// License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with OP25; see the file COPYING. If not, write to the Free
// Software Foundation, Inc., 51 Franklin Street, Boston, MA
// 02110-1301, USA.

#include "p25p2_isch.h"
struct entry { uint64_t word; int16_t value; };
static const entry words[] = {
{0x184229d461ULL,0},
{0x18761451f6ULL,1},
{0x181ae27e2fULL,2},
{0x182edffbb8ULL,3},
{0x18df8a7510ULL,4},
{0x18ebb7f087ULL,5},
{0x188741df5eULL,6},
{0x18b37c5ac9ULL,7},
{0x1146a44f13ULL,8},
{0x117299ca84ULL,9},
{0x111e6fe55dULL,10},
{0x112a5260caULL,11},
{0x11db07ee62ULL,12},
{0x11ef3a6bf5ULL,13},
{0x1183cc442cULL,14},
{0x11b7f1c1bbULL,15},
{0x1a4a2e239eULL,16},
{0x1a7e13a609ULL,17},
{0x1a12e589d0ULL,18},
{0x1a26d80c47ULL,19},
{0x1ad78d82efULL,20},
{0x1ae3b00778ULL,21},
{0x1a8f4628a1ULL,22},
{0x1abb7bad36ULL,23},
{0x134ea3b8ecULL,24},
{0x137a9e3d7bULL,25},
{0x13166812a2ULL,26},
{0x1322559735ULL,27},
{0x13d300199dULL,28},
{0x13e73d9c0aULL,29},
{0x138bcbb3d3ULL,30},
{0x13bff63644ULL,31},
{0x1442f705efULL,32},
{0x1476ca8078ULL,33},
{0x141a3cafa1ULL,34},
{0x142e012a36ULL,35},
{0x14df54a49eULL,36},
{0x14eb692109ULL,37},
{0x14879f0ed0ULL,38},
{0x14b3a28b47ULL,39},
{0x1d467a9e9dULL,40},
{0x1d72471b0aULL,41},
{0x1d1eb134d3ULL,42},
{0x1d2a8cb144ULL,43},
{0x1ddbd93fecULL,44},
{0x1defe4ba7bULL,45},
{0x1d831295a2ULL,46},
{0x1db72f1035ULL,47},
{0x164af0f210ULL,48},
{0x167ecd7787ULL,49},
{0x16123b585eULL,50},
{0x162606ddc9ULL,51},
{0x16d7535361ULL,52},
{0x16e36ed6f6ULL,53},
{0x168f98f92fULL,54},
{0x16bba57cb8ULL,55},
{0x1f4e7d6962ULL,56},
{0x1f7a40ecf5ULL,57},
{0x1f16b6c32cULL,58},
{0x1f228b46bbULL,59},
{0x1fd3dec813ULL,60},
{0x1fe7e34d84ULL,61},
{0x1f8b15625dULL,62},
{0x1fbf28e7caULL,63},
{0x084d62c339ULL,64},
{0x08795f46aeULL,65},
{0x0815a96977ULL,66},
{0x082194ece0ULL,67},
{0x08d0c16248ULL,68},
{0x08e4fce7dfULL,69},
{0x08880ac806ULL,70},
{0x08bc374d91ULL,71},
{0x0149ef584bULL,72},
{0x017dd2dddcULL,73},
{0x011124f205ULL,74},
{0x0125197792ULL,75},
{0x01d44cf93aULL,76},
{0x01e0717cadULL,77},
{0x018c875374ULL,78},
{0x01b8bad6e3ULL,79},
{0x0a456534c6ULL,80},
{0x0a7158b151ULL,81},
{0x0a1dae9e88ULL,82},
{0x0a29931b1fULL,83},
{0x0ad8c695b7ULL,84},
{0x0aecfb1020ULL,85},
{0x0a800d3ff9ULL,86},
{0x0ab430ba6eULL,87},
{0x0341e8afb4ULL,88},
{0x0375d52a23ULL,89},
{0x03192305faULL,90},
{0x032d1e806dULL,91},
{0x03dc4b0ec5ULL,92},
{0x03e8768b52ULL,93},
{0x038480a48bULL,94},
{0x03b0bd211cULL,95},
{0x044dbc12b7ULL,96},
{0x0479819720ULL,97},
{0x041577b8f9ULL,98},
{0x04214a3d6eULL,99},
{0x04d01fb3c6ULL,100},
{0x04e4223651ULL,101},
{0x0488d41988ULL,102},
{0x04bce99c1fULL,103},
{0x0d493189c5ULL,104},
{0x0d7d0c0c52ULL,105},
{0x0d11fa238bULL,106},
{0x0d25c7a61cULL,107},
{0x0dd49228b4ULL,108},
{0x0de0afad23ULL,109},
{0x0d8c5982faULL,110},
{0x0db864076dULL,111},
{0x0645bbe548ULL,112},
{0x06718660dfULL,113},
{0x061d704f06ULL,114},
{0x06294dca91ULL,115},
{0x06d8184439ULL,116},
{0x06ec25c1aeULL,117},
{0x0680d3ee77ULL,118},
{0x06b4ee6be0ULL,119},
{0x0f41367e3aULL,120},
{0x0f750bfbadULL,121},
{0x0f19fdd474ULL,122},
{0x0f2dc051e3ULL,123},
{0x0fdc95df4bULL,124},
{0x0fe8a85adcULL,125},
{0x0f845e7505ULL,126},
{0x0fb063f092ULL,127},
{0x575d57f7ffULL,-2},
};
p25p2_isch::p25p2_isch() {}
int16_t p25p2_isch::isch_lookup(const uint8_t dibits[]) {
    uint64_t word=0; for(int i=0;i<20;i++)word=(word<<2)|(dibits[i]&3);
    return isch_lookup(word);
}
int16_t p25p2_isch::isch_lookup(uint64_t word) {
    int best=8; int16_t value=-1;
    for(const auto &e:words) {
        int distance=__builtin_popcountll(e.word^word);
        if(distance<best){best=distance;value=e.value;}
    }
    return value;
}
