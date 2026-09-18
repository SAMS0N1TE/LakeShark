#pragma once

/* `lora pocsag` and `lora fsk` - receive. */
int sx1262_receive_command(int argc, char **argv);

/* `lora fsktx` - transmit one arbitrary 2-FSK frame. Carrier, rate,
   deviation, filter, sync word, preamble length, power and payload are all
   arguments, so this is the radio's transmit capability rather than any one
   protocol's helper. */
int sx1262_transmit_command(int argc, char **argv);

/* `lora fskls`, `lora fskplay`, `lora fsksave` - the captures the FSK
   receiver filed: list them, put one back on air, or write one out as a
   Flipper `.sub` with a custom 2-FSK preset. */
int sx1262_capture_command(int argc, char **argv);
