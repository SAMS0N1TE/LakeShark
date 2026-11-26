/* LR433's receive sessions, in one place: the sampler settings for each kind
   of sub-GHz sensor the LoRa chip listens for. LR433 (exp_lr433.c's PLAN)
   and WARDRIVE's radio jobs (wd_feed_board.c) both build their sessions from
   these, so a retune here reaches both.

   Each expands to, in order: decisions a second, filter (Hz), FSK deviation
   (Hz, 0 for OOK), sync word or OOK detector pattern, its bits, what goes
   back in front of the payload, its bits, payload bytes.

   OOK: a quiet decision then 3 high ones, the start of any burst whose first
   pulse is at least three decisions long. FSK: 0x0F0F, two cycles of an
   alternating preamble four decisions a chip, which reads the same in either
   polarity. The 19.2k TPMS session hears the 20k ones too: the chip's clock
   recovery follows a few percent. Header only: two modules use it. */
#ifndef LR433_PLAN_H
#define LR433_PLAN_H

/* TPMS, OOK: Schrader, GM, Subaru (315) */
#define LR433_P_OOK_TPMS  40000, 200000, 0, 0x000E, 4, 0x7, 4, 252
/* TPMS, FSK: Ford, Toyota, Hyundai/Kia (315); Citroen/VDO, Ford, Toyota (433) */
#define LR433_P_FSK_TPMS  76800, 250000, 38000, 0x0F0F, 16, 0x0F0F, 16, 128
/* TPMS, Toyota/Lexus PMV-107J: the rest of its preamble goes back in front */
#define LR433_P_FSK_PMV   40000, 166667, 25000, 0xFFF0, 16, 0xFFFFFF0, 28, 72
/* weather stations, OOK: Acurite, LaCrosse, Ambient F007 (433) */
#define LR433_P_OOK_WX    16000, 250000, 0, 0x000E, 4, 0x7, 4, 252
/* weather stations, FSK: Ambient/Ecowitt WH31, WH24/65 (915/868) */
#define LR433_P_FSK_WX    68966, 250000, 35000, 0x0F0F, 16, 0x0F0F, 16, 112

#endif
