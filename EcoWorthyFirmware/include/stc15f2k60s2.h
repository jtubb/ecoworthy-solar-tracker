#ifndef STC15F2K60S2_H
#define STC15F2K60S2_H

/*
 * Minimal SFR header for STC15F2K60S2 (SOP28). SDCC 8051 syntax.
 * Addresses verified against the STC15 datasheet rev. 2024.
 * Only SFRs we actually use are declared — extend as features land.
 */

/* ---- Ports (all bit-addressable on STC15) ---- */
__sfr __at (0x80) P0;
__sfr __at (0x90) P1;
__sfr __at (0xA0) P2;
__sfr __at (0xB0) P3;
__sfr __at (0xC0) P4;
__sfr __at (0xC8) P5;

/* ---- Port mode registers (STC15 extension over classic 8051) ----
 * Each pin has a 2-bit mode: {PnM1[i], PnM0[i]}
 *   00 = quasi-bidirectional (8051 default, weak pull-up)
 *   01 = push-pull output
 *   10 = high-impedance input
 *   11 = open-drain
 */
/*
 * Port-mode register addresses verified against STC15 datasheet SFR map.
 * Note: P*M1 is the LOW-numbered address, P*M0 is the HIGH-numbered
 * one -- the opposite of what naive numerical ordering suggests.
 * Mode encoding {P*M1[i], P*M0[i]}:
 *   00 = quasi-bidirectional (8051 default)
 *   01 = push-pull
 *   10 = high-impedance input
 *   11 = open-drain
 */
__sfr __at (0x93) P0M1;
__sfr __at (0x94) P0M0;
__sfr __at (0x91) P1M1;
__sfr __at (0x92) P1M0;
__sfr __at (0x95) P2M1;
__sfr __at (0x96) P2M0;
__sfr __at (0xB1) P3M1;
__sfr __at (0xB2) P3M0;
__sfr __at (0xB3) P4M1;
__sfr __at (0xB4) P4M0;
__sfr __at (0xC9) P5M1;
__sfr __at (0xCA) P5M0;

/* ---- Clock / power ---- */
__sfr __at (0x8E) AUXR;       /* timer-clock / UART / EXTRAM */
__sfr __at (0x97) CLK_DIV;    /* also PCON2; bit 5 = ADRJ (ADC right-justify) */

/* ---- ADC (10-bit SAR, 8 channels on P1) ---- */
__sfr __at (0x9D) P1ASF;      /* per-bit analog-function enable for P1 */
__sfr __at (0xBC) ADC_CONTR;  /* power/speed/flag/start/channel */
__sfr __at (0xBD) ADC_RES;    /* result high byte */
__sfr __at (0xBE) ADC_RESL;   /* result low byte */

/* ---- Timer 0 (used later for software-UART bit timing) ---- */
__sfr __at (0x88) TCON;
__sfr __at (0x89) TMOD;
__sfr __at (0x8A) TL0;
__sfr __at (0x8C) TH0;

/* ---- Interrupts ---- */
__sfr __at (0xA8) IE;         /* EA, ELVD, EADC, ES, ET1, EX1, ET0, EX0 */

/* TCON bits (bit-addressable @ 0x88) */
__sbit __at (0x88) IT0;       /* INT0 trigger: 1 = falling-edge, 0 = both edges */
__sbit __at (0x89) IE0;       /* INT0 latch */
__sbit __at (0x8A) IT1;
__sbit __at (0x8B) IE1;
__sbit __at (0x8C) TR0;       /* Timer 0 run */
__sbit __at (0x8D) TF0;       /* Timer 0 overflow */
__sbit __at (0x8E) TR1;
__sbit __at (0x8F) TF1;

/* IE bits (bit-addressable @ 0xA8) */
__sbit __at (0xA8) EX0;       /* enable INT0 */
__sbit __at (0xA9) ET0;
__sbit __at (0xAA) EX1;
__sbit __at (0xAB) ET1;
__sbit __at (0xAC) ES;
__sbit __at (0xAF) EA;        /* global interrupt enable */

#endif /* STC15F2K60S2_H */
