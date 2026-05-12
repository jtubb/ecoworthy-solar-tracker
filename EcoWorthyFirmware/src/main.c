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

/*
 * HD44780 1602A LCD driver -- 8-bit parallel, D0..D7 on P2, RS/RW/E on
 * P3.5/P3.6/P3.7.  Write-only (RW always 0); we never read the busy
 * flag and instead just wait the worst-case time for each command.
 */
static void lcd_pulse_e(void) {
    LCD_E = 1;
    delay_us(2);     /* HD44780 needs E HIGH for >=230 ns */
    LCD_E = 0;
    delay_us(2);
}

static void lcd_cmd(unsigned char cmd) {
    LCD_RS = 0;
    LCD_RW = 0;
    LCD_DATA = cmd;
    lcd_pulse_e();
    delay_us(50);    /* most commands complete in 37 us; 50 us is safe */
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
    /* Power-on settling time. */
    delay_ms(50);

    /*
     * Three-step wake-up forcing 8-bit mode regardless of prior state.
     * Bypass lcd_cmd() because the first writes need bespoke timing.
     */
    LCD_RS = 0;
    LCD_RW = 0;

    LCD_DATA = 0x30;
    lcd_pulse_e();
    delay_ms(5);

    LCD_DATA = 0x30;
    lcd_pulse_e();
    delay_us(150);

    LCD_DATA = 0x30;
    lcd_pulse_e();
    delay_us(50);

    /* Function set: 8-bit, 2 lines, 5x8 font. */
    lcd_cmd(0x38);
    /* Display OFF. */
    lcd_cmd(0x08);
    /* Clear display (slow command, needs 1.5 ms). */
    lcd_cmd(0x01);
    delay_ms(2);
    /* Entry mode: increment cursor, no display shift. */
    lcd_cmd(0x06);
    /* Display ON, cursor OFF, blink OFF. */
    lcd_cmd(0x0C);
}

static void lcd_goto(unsigned char row, unsigned char col) {
    /* HD44780 DDRAM: row 0 starts at 0x00, row 1 at 0x40. */
    unsigned char addr = (row ? 0x40 : 0x00) + col;
    lcd_cmd(0x80 | addr);   /* 0x80 = "set DDRAM address" command bit */
}

void main(void) {
    /* Relay-safe boot. BUZZER is active-LOW so HIGH = silent. */
    P3 = 0x00;
    P5 &= ~(1 << 4);
    BUZZER = 1;

    /*
     * Pin modes -- push-pull on every output we drive:
     *   P1.7        = BUZZER
     *   P3.0, .1, .3 = relays E, S, W
     *   P3.5, .6, .7 = LCD RS, RW, E
     *   P5.4        = relay N
     *   P2.0..P2.7  = LCD data bus D0..D7
     */
    P3M0 |=  (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7);
    P3M1 &= ~((1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7));
    P5M0 |=  (1 << 4);
    P5M1 &= ~(1 << 4);
    P1M0 |=  (1 << 7);
    P1M1 &= ~(1 << 7);
    P2M0 = 0xFF;
    P2M1 = 0x00;

    /* Bring up the LCD. */
    lcd_init();
    lcd_goto(0, 0);
    lcd_print("EcoWorthy");
    lcd_goto(1, 0);
    lcd_print("Phase 0 OK");

    for (;;) {
    }
}
