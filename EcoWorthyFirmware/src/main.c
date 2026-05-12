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

/* Print an unsigned 16-bit as 3 digits, zero-padded ("000".."999").
 * Clamps to 999 -- ADC max of 1023 displays as "999", losing the top
 * 24 values, which we accept for layout. */
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

#define ANALOG_PINS   ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 6))

static void adc_init(void) {
    CLK_DIV |= (1 << 5);       /* ADRJ = 1 */
    ADC_CONTR = ADC_POWER;
    delay_ms(1);
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

/* ---- Button decoder (thresholds are midpoints between captured readings). */
typedef enum {
    BTN_NONE = 0,
    BTN_SET,
    BTN_QUIT,
    BTN_WEST,
    BTN_EAST,
    BTN_NORTH,
    BTN_SOUTH
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

/* 6-character label, space-padded so writes always overwrite cleanly. */
static const char *button_label(button_t b) {
    switch (b) {
        case BTN_SET:   return "SET   ";
        case BTN_QUIT:  return "QUIT  ";
        case BTN_WEST:  return "WEST  ";
        case BTN_EAST:  return "EAST  ";
        case BTN_NORTH: return "NORTH ";
        case BTN_SOUTH: return "SOUTH ";
        default:        return "      ";
    }
}

void main(void) {
    /* Relay-safe boot. */
    P3 = 0x00;
    P5 &= ~(1 << 4);
    BUZZER = 1;

    /* Push-pull outputs. */
    P3M0 |=  (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7);
    P3M1 &= ~((1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7));
    P5M0 |=  (1 << 4);
    P5M1 &= ~(1 << 4);
    P1M0 |=  (1 << 7);     /* BUZZER push-pull */
    P1M1 &= ~(1 << 7);
    P2M0 = 0xFF;
    P2M1 = 0x00;

    /* P1.0-P1.3 (sun sensors) and P1.6 (button bus) as analog inputs
     * (high-Z digital mode + P1ASF analog-function enable). */
    P1M1 |=  ANALOG_PINS;
    P1M0 &= ~ANALOG_PINS;
    P1ASF = ANALOG_PINS;

    lcd_init();
    adc_init();

    for (;;) {
        unsigned int sun_e = adc_read(ADC_CH_SUN_E);
        unsigned int sun_w = adc_read(ADC_CH_SUN_W);
        unsigned int sun_s = adc_read(ADC_CH_SUN_S);
        unsigned int sun_n = adc_read(ADC_CH_SUN_N);
        unsigned int btn_v = adc_read(ADC_CH_BUTTONS);
        button_t b = button_classify(btn_v);

        /* Line 1: "N999 S999 E999  "  -- three sensors here, W on line 2.
         * Raw ADC (0..999) so small solar-cell millivolt swings are visible. */
        lcd_goto(0, 0);
        lcd_putc('N'); lcd_print_u16_3d(sun_n); lcd_putc(' ');
        lcd_putc('S'); lcd_print_u16_3d(sun_s); lcd_putc(' ');
        lcd_putc('E'); lcd_print_u16_3d(sun_e); lcd_putc(' ');
        lcd_putc(' ');

        /* Line 2: "W999 Btn:NORTH " */
        lcd_goto(1, 0);
        lcd_putc('W'); lcd_print_u16_3d(sun_w); lcd_putc(' ');
        lcd_print("Btn:");
        lcd_print(button_label(b));

        delay_ms(50);
    }
}
