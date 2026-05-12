#include "board.h"

static void delay_us(unsigned int us) {
    while (us--) {
        volatile unsigned char i = 4;
        while (i--) { }
    }
}

static void delay_ms(unsigned int ms) {
    while (ms--) delay_us(1000);
}

/* ---- HD44780 1602A LCD ---- */
static void lcd_pulse_e(void) {
    LCD_E = 1;
    delay_us(2);
    LCD_E = 0;
    delay_us(2);
}

static void lcd_cmd(unsigned char cmd) {
    LCD_RS = 0;
    LCD_RW = 0;
    LCD_DATA = cmd;
    lcd_pulse_e();
    delay_us(50);
}

static void lcd_putc(char c) {
    LCD_RS = 1;
    LCD_RW = 0;
    LCD_DATA = (unsigned char)c;
    lcd_pulse_e();
    delay_us(50);
}

static void lcd_print(const char *s) {
    while (*s) lcd_putc(*s++);
}

static void lcd_init(void) {
    delay_ms(50);
    LCD_RS = 0;
    LCD_RW = 0;

    LCD_DATA = 0x30; lcd_pulse_e(); delay_ms(5);
    LCD_DATA = 0x30; lcd_pulse_e(); delay_us(150);
    LCD_DATA = 0x30; lcd_pulse_e(); delay_us(50);

    lcd_cmd(0x38);
    lcd_cmd(0x08);
    lcd_cmd(0x01);
    delay_ms(2);
    lcd_cmd(0x06);
    lcd_cmd(0x0C);
}

static void lcd_goto(unsigned char row, unsigned char col) {
    unsigned char addr = (row ? 0x40 : 0x00) + col;
    lcd_cmd(0x80 | addr);
}

static void lcd_print_u16_3d(unsigned int v) {
    if (v > 999) v = 999;
    lcd_putc('0' + (v / 100));
    lcd_putc('0' + ((v / 10) % 10));
    lcd_putc('0' + (v % 10));
}

/* ---- STC15 ADC ---- */
#define ADC_POWER     0x80
#define ADC_SPEED_540 0x00
#define ADC_FLAG      0x10
#define ADC_START     0x08

#define ANALOG_PINS   ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6))

static void adc_init(void) {
    CLK_DIV |= (1 << 5);
    ADC_CONTR = ADC_POWER;
    delay_ms(1);
}

/* Convert a 10-bit ADC reading from the wind sensor (0-2V output,
 * 0-50 m/s range, slope 25 m/s per volt) to m/s.  Math:
 *     m/s = (ADC / 1023) * 5V * 25 = ADC * 125 / 1023
 * Capped at 99 to keep the display 2-digit.  Realistic max is ~50. */
static unsigned char wind_mps(unsigned int adc) {
    unsigned long v = (unsigned long)adc * 125UL / 1023UL;
    return (v > 99) ? 99 : (unsigned char)v;
}

static unsigned int adc_read(unsigned char channel) {
    ADC_CONTR = ADC_POWER | ADC_SPEED_540 | ADC_START | (channel & 0x07);
    __asm
        nop
        nop
        nop
        nop
    __endasm;
    while (!(ADC_CONTR & ADC_FLAG)) { }
    ADC_CONTR &= ~ADC_FLAG;
    return ((unsigned int)(ADC_RES & 0x03) << 8) | ADC_RESL;
}

/* ---- Button decoder ---- */
typedef enum {
    BTN_NONE = 0, BTN_SET, BTN_QUIT, BTN_WEST, BTN_EAST, BTN_NORTH, BTN_SOUTH
} button_t;

static button_t button_classify(unsigned int adc) {
    if (adc < 94)   return BTN_NONE;
    if (adc < 271)  return BTN_SET;
    if (adc < 436)  return BTN_QUIT;
    if (adc < 589)  return BTN_WEST;
    if (adc < 748)  return BTN_EAST;
    if (adc < 929)  return BTN_NORTH;
    return BTN_SOUTH;
}

/* ---- Axis state machine ----
 * Empirically determined H-bridge pairing on this board (Phase 2A test):
 *   N/S axis (tilt actuator):  RELAY_N (extend) + RELAY_W (retract)
 *   E/W axis (rotate actuator): RELAY_S (extend) + RELAY_E (retract)
 *
 * Note: cardinal button labels do NOT match cardinal axis pairing on
 * this installation -- pressing N+W controls the SAME actuator, as does
 * S+E.  Mutex must protect these real pairs, not the intuitive {N,S}
 * and {E,W} groupings.
 */
typedef enum {
    AXIS_OFF = 0,
    AXIS_FWD,    /* extends: N for tilt, S for rotate */
    AXIS_REV     /* retracts: W for tilt, E for rotate */
} axis_state_t;

static axis_state_t ns_state = AXIS_OFF;   /* tilt: RELAY_N (FWD), RELAY_W (REV) */
static axis_state_t ew_state = AXIS_OFF;   /* rotate: RELAY_S (FWD), RELAY_E (REV) */

static void set_axis_ns(axis_state_t target) {
    if (target == ns_state) return;
    RELAY_N = 0;
    RELAY_W = 0;
    if (ns_state != AXIS_OFF && target != AXIS_OFF) {
        delay_ms(10);
    }
    if (target == AXIS_FWD) RELAY_N = 1;
    if (target == AXIS_REV) RELAY_W = 1;
    ns_state = target;
}

static void set_axis_ew(axis_state_t target) {
    if (target == ew_state) return;
    RELAY_S = 0;
    RELAY_E = 0;
    if (ew_state != AXIS_OFF && target != AXIS_OFF) {
        delay_ms(10);
    }
    if (target == AXIS_FWD) RELAY_S = 1;
    if (target == AXIS_REV) RELAY_E = 1;
    ew_state = target;
}

/* Active button letter, '-' if axis is off. */
static char ns_char(axis_state_t s) {
    if (s == AXIS_FWD) return 'N';
    if (s == AXIS_REV) return 'S';
    return '-';
}
static char ew_char(axis_state_t s) {
    if (s == AXIS_FWD) return 'E';
    if (s == AXIS_REV) return 'W';
    return '-';
}

void main(void) {
    /* Relay-safe boot. */
    P3 = 0x00;
    P5 &= ~(1 << 4);
    BUZZER = 1;

    P3M0 |=  (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7);
    P3M1 &= ~((1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7));
    P5M0 |=  (1 << 4);
    P5M1 &= ~(1 << 4);
    P1M0 |=  (1 << 7);
    P1M1 &= ~(1 << 7);
    P2M0 = 0xFF;
    P2M1 = 0x00;

    P1M1 |=  ANALOG_PINS;
    P1M0 &= ~ANALOG_PINS;
    P1ASF = ANALOG_PINS;

    lcd_init();
    adc_init();

    for (;;) {
        unsigned int sun_n = adc_read(ADC_CH_SUN_N);
        unsigned int sun_s = adc_read(ADC_CH_SUN_S);
        unsigned int sun_e = adc_read(ADC_CH_SUN_E);
        unsigned int sun_w = adc_read(ADC_CH_SUN_W);
        unsigned int wind  = adc_read(ADC_CH_WIND);
        unsigned int btn_v = adc_read(ADC_CH_BUTTONS);
        button_t b = button_classify(btn_v);

        /* Button -> axis target.  Cardinal-intuitive mapping: N/S buttons
         * both drive the tilt axis, E/W buttons both drive the rotate
         * axis.  The relay labels don't help here -- physical H-bridge
         * pairs are {RELAY_N, RELAY_W} and {RELAY_S, RELAY_E}, so the
         * S-button routes to RELAY_W and the W-button to RELAY_S. */
        axis_state_t tgt_ns = AXIS_OFF;
        axis_state_t tgt_ew = AXIS_OFF;
        switch (b) {
            case BTN_NORTH: tgt_ns = AXIS_FWD; break;  /* RELAY_N -> extend -> tilt N */
            case BTN_SOUTH: tgt_ns = AXIS_REV; break;  /* RELAY_W -> retract -> tilt S */
            case BTN_EAST:  tgt_ew = AXIS_FWD; break;  /* RELAY_S -> extend -> rotate E */
            case BTN_WEST:  tgt_ew = AXIS_REV; break;  /* RELAY_E -> retract -> rotate W */
            default: break;
        }
        set_axis_ns(tgt_ns);
        set_axis_ew(tgt_ew);

        /* Line 1: "N999S999E999W999"  -- sun sensors. */
        lcd_goto(0, 0);
        lcd_putc('N'); lcd_print_u16_3d(sun_n);
        lcd_putc('S'); lcd_print_u16_3d(sun_s);
        lcd_putc('E'); lcd_print_u16_3d(sun_e);
        lcd_putc('W'); lcd_print_u16_3d(sun_w);

        /* Line 2: "NS=N EW=E Wnd=50"  -- axis states + wind speed (m/s). */
        {
            unsigned char mps = wind_mps(wind);
            lcd_goto(1, 0);
            lcd_print("NS=");
            lcd_putc(ns_char(ns_state));
            lcd_print(" EW=");
            lcd_putc(ew_char(ew_state));
            lcd_print(" Wnd=");
            lcd_putc('0' + (mps / 10));
            lcd_putc('0' + (mps % 10));
        }

        delay_ms(50);
    }
}
