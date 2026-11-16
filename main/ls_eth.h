#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ls_eth_start(void);
int ls_eth_command(int argc, char **argv);

/* L2 tap on the wired port (Nano EMAC or USB adapter): every Ethernet frame
   handed to lwIP (tx=false) and every frame the driver accepted for sending
   (tx=true), FCS excluded, before lwIP sees it. Runs in the driver's receive
   task / lwIP's thread: copy what is needed and return, never block. NULL
   removes it. One tap at a time.

   Meant for the capture layer: LAP_CAPTURE_ETH (being added to the app SDK)
   registers here to record the wired port as pcap DLT_EN10MB. Not wired up
   yet. */
typedef void (*ls_eth_tap_fn)(const uint8_t *frame, size_t len, bool tx, void *ctx);
void ls_eth_set_l2_tap(ls_eth_tap_fn fn, void *ctx);
