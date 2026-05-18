#ifndef BOARD_H
#define BOARD_H

#include "stc15f2k60s2.h"

/*
 * Pin map for the Ecoworthy dual-axis solar tracker PCB.
 * SOP28 pin numbers in comments come from the STC15F2K60S2 datasheet
 * cross-referenced with SolarTracker/Ecoworthy Board Description.txt.
 * See CLAUDE.md "Verified pin-to-port map" for the full table.
 */

/* ---- System clock ---- */
#define F_CPU 22118400UL   /* 22.1184 MHz internal RC; baud-rate friendly */

/* ---- Buzzer (pin 10, P1.7) ----
 * Active-LOW: BUZZER = 0 sounds, BUZZER = 1 silences. The board wires the
 * piezo to the 5 V rail with pin 10 as the sink-side switch.
 */
__sbit __at (0x97) BUZZER;

/* ---- Relays (active HIGH at the STC pin -> ULN2003A energizes coil) ----
 * Pairs form a 2-relay H-bridge per axis. MUTUAL EXCLUSION REQUIRED per axis:
 *   - E/W axis: RELAY_E and RELAY_W
 *   - N/S axis: RELAY_N and RELAY_S
 * RELAY_N is on P5.4, which is also the RST pin -- requires ENRST=off
 * in the stcgal flash options. See CLAUDE.md quirk 8.
 */
__sbit __at (0xB0) RELAY_E;   /* pin 15, P3.0  (relay 2, vendor "East") */
__sbit __at (0xB1) RELAY_S;   /* pin 16, P3.1  (relay 4, vendor "South") */
__sbit __at (0xB3) RELAY_W;   /* pin 18, P3.3  (relay 1, vendor "West") */
__sbit __at (0xCC) RELAY_N;   /* pin 11, P5.4  (relay 3, vendor "North") */

/* ---- ESP bridge / IR receiver (pin 17, P3.2, INT0) ----
 * Open-drain, idle HIGH (breakout supplies pull-up).
 */
__sbit __at (0xB2) BRIDGE_IO;

/* ---- LCD (HD44780 on 1602A, 8-bit parallel) ----
 * D0..D7 land on P2.0..P2.7 (D0..D5 on pins 23..28, D6..D7 on pins 1..2).
 * Convenient: a whole-port write to P2 sets the entire data bus at once.
 */
#define LCD_DATA  P2
__sbit __at (0xB4) LCD_BL;    /* pin 19, P3.4 -- backlight, active-HIGH */
__sbit __at (0xB5) LCD_RS;    /* pin 20, P3.5 */
__sbit __at (0xB6) LCD_RW;    /* pin 21, P3.6 */
__sbit __at (0xB7) LCD_E;     /* pin 22, P3.7 */

/* ---- ADC bank (P1.0..P1.6 = ADC0..ADC6) ----
 * Defined as channel numbers for future ADC code; no sbits needed.
 */
#define ADC_CH_SUN_E      0   /* pin 3 */
#define ADC_CH_SUN_W      1   /* pin 4 */
#define ADC_CH_SUN_S      2   /* pin 5 */
#define ADC_CH_SUN_N      3   /* pin 6 */
#define ADC_CH_WIND       4   /* pin 7  -- analog wind sensor, 0-2V (Vout * 25 = m/s) */
#define ADC_CH_STALL      5   /* pin 8  -- soft current sense for stall detect */
#define ADC_CH_BUTTONS    6   /* pin 9  -- resistor-ladder button bus */

#endif /* BOARD_H */
