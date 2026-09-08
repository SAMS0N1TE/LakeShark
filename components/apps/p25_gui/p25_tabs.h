#ifndef P25_TABS_H
#define P25_TABS_H

/* LS-736: the P25 screen's fixed vocabulary - tab names and the DECODE action
 * row - lives here rather than as string literals inside run() and
 * buildDecodeTab(), because bench/tests/test_p25_screen_fit.cpp measures these
 * exact strings against the shared tab strip and the shared action row on both
 * panels.  A label renamed in the app and not in the test would prove nothing,
 * so there is one list and both sides read it.
 *
 * Deliberately no LVGL, no C++ and no other include: the bench compiles this
 * beside a host LVGL build with none of the firmware around it. */

#define P25_TAB_DECODE  0
#define P25_TAB_SIGNAL  1
#define P25_TAB_HEALTH  2
#define P25_TAB_SCAN    3
#define P25_TAB_CONFIG  4
#define P25_TAB_PROGRAM 5
#define P25_TAB_GROUPS  6
#define P25_TAB_COUNT   7

#define P25_TAB_NAMES { \
    "DECODE", "SIGNAL", "HEALTH", "SCAN", "CONFIG", "PROGRAM", "TALK GROUPS" }

#define P25_DECODE_ACTION_COUNT 7

#define P25_DECODE_ACTIONS { \
    "C-SCAN", "HOLD", "LOCK", "MODE", "AGC", "RESET", "BEEP" }

/* The same row once the scanner is running, a talkgroup is held and the sync
 * beep is on.  updateDecode rewrites three of these labels in place, and the
 * held-talkgroup form is the widest text the row ever carries - a fit measured
 * only against the built labels would not have seen it. */
#define P25_DECODE_ACTIONS_WIDE { \
    "C-STOP", "HLD65535", "LOCK", "MODE", "AGC", "RESET", "BEEP*" }

#endif
