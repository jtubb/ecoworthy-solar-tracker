#include "board.h"
#include <string.h>

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

static void lcd_print_padded(const char *s, unsigned char width) {
    unsigned char n = 0;
    while (*s && n < width) {
        lcd_putc(*s++);
        n++;
    }
    while (n < width) {
        lcd_putc(' ');
        n++;
    }
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

    /* Define custom glyph 0 = padlock (duty-lockout indicator).
     * CGRAM addr = 0x40 | (slot<<3); 8 rows, low 5 bits each. */
    lcd_cmd(0x40);
    lcd_putc(0x0E);  /*  .###.  shackle */
    lcd_putc(0x11);  /*  #...#         */
    lcd_putc(0x11);  /*  #...#         */
    lcd_putc(0x1F);  /*  #####  body   */
    lcd_putc(0x1B);  /*  ##.##  keyhole*/
    lcd_putc(0x1B);  /*  ##.##         */
    lcd_putc(0x1F);  /*  #####         */
    lcd_putc(0x00);  /*  .....         */
    lcd_cmd(0x80);   /* return addr ptr to DDRAM home */
}

static void lcd_goto(unsigned char row, unsigned char col) {
    unsigned char addr = (row ? 0x40 : 0x00) + col;
    lcd_cmd(0x80 | addr);
}

static void lcd_clear(void) {
    lcd_cmd(0x01);
    delay_ms(2);
}

/* ---- 1 kHz tick timer for accurate elapsed-time tracking ----
 * Timer 0 in mode 1 (16-bit, manual reload) at 1 ms.  Increments a
 * 32-bit ms_count.  Used by millis() for stroke timing, stall detection
 * window, duty-cycle decay, etc.  Loop-iteration approximations remain
 * fine for non-critical waits (delay_ms above).
 */
static volatile unsigned long ms_count = 0;

#define T0_RELOAD_LO ((unsigned char)((65536 - 22118) & 0xFF))
#define T0_RELOAD_HI ((unsigned char)((65536 - 22118) >> 8))

void timer0_isr(void) __interrupt(1) {
    TL0 = T0_RELOAD_LO;
    TH0 = T0_RELOAD_HI;
    ms_count++;
}

static void timer0_init(void) {
    AUXR |= (1 << 7);               /* T0 in 1T mode */
    TMOD = (TMOD & 0xF0) | 0x01;    /* T0 mode 1 (16-bit) */
    TL0 = T0_RELOAD_LO;
    TH0 = T0_RELOAD_HI;
    ET0 = 1;
    EA = 1;
    TR0 = 1;
}

/* Atomic 32-bit read against the ISR. */
static unsigned long millis(void) {
    unsigned long m;
    EA = 0;
    m = ms_count;
    EA = 1;
    return m;
}

/* ---- Software UART TX (Phase 3-1) ----
 * Half-duplex bridge on P3.2 (BRIDGE_IO), open-drain, 9600 8N1.
 * Timer 1 free-runs at the bit rate (104.166 us); its ISR shifts the
 * TX state machine.  RX (INT0-driven) lands in P3-2 and will share
 * this Timer 1.  9600 baud at 22.1184 MHz 1T = exactly 2304 ticks/bit
 * (the crystal was chosen for integer baud divisors).
 *
 * Open-drain semantics on the shared bus: writing 1 = release (the
 * breakout pull-up makes the line HIGH = UART idle / mark / data '1');
 * writing 0 = actively pull LOW (start bit / data '0').
 */
#define T1_TICKS_PER_BIT  2304U
#define T1_RELOAD         ((unsigned int)(65536U - T1_TICKS_PER_BIT))
#define T1_RELOAD_LO      ((unsigned char)(T1_RELOAD & 0xFF))
#define T1_RELOAD_HI      ((unsigned char)(T1_RELOAD >> 8))

/* 1.5 bit-times: used once, from the INT0 start-bit edge, so the first
 * Timer 1 fire lands in the MIDDLE of data bit 0.  Subsequent fires
 * use the normal 1-bit reload -> every sample stays mid-bit. */
#define T1_TICKS_1P5      3456U
#define T1_RELOAD_1P5     ((unsigned int)(65536U - T1_TICKS_1P5))
#define T1_RELOAD_1P5_LO  ((unsigned char)(T1_RELOAD_1P5 & 0xFF))
#define T1_RELOAD_1P5_HI  ((unsigned char)(T1_RELOAD_1P5 >> 8))

#define UART_TX_SZ   64           /* ring buffer; power of two */
#define UART_TX_MASK (UART_TX_SZ - 1)
#define UART_RX_SZ   64
#define UART_RX_MASK (UART_RX_SZ - 1)

/* Rings in __xdata: 128 bytes of buffer won't fit the 128-byte IRAM.
 * ISR xdata access costs a few MOVX cycles -- fine at 104 us/bit. */
static volatile __xdata unsigned char uart_tx_ring[UART_TX_SZ];
static volatile unsigned char uart_tx_head = 0;   /* writer (main) */
static volatile unsigned char uart_tx_tail = 0;   /* reader (ISR)  */
/* TX bit state: 0 = idle, 1..8 = data bit 0..7, 9 = stop bit. */
static volatile unsigned char uart_tx_state = 0;
static volatile unsigned char uart_tx_shift = 0;
/* Set while a TX burst owns the bus -> INT0 (RX listen) is disabled
 * for its whole duration, re-armed once the ring drains. */
static volatile unsigned char uart_tx_engaged = 0;

static volatile __xdata unsigned char uart_rx_ring[UART_RX_SZ];
static volatile unsigned char uart_rx_head = 0;   /* writer (ISR)  */
static volatile unsigned char uart_rx_tail = 0;   /* reader (main) */
/* RX bit state: rx_active gated by INT0; rx_bit 0..7 = data, 8 = stop. */
static volatile unsigned char uart_rx_active = 0;
static volatile unsigned char uart_rx_bit   = 0;
static volatile unsigned char uart_rx_shift = 0;

/* INT0 (pin 17 falling edge) = incoming start bit.  Only reached when
 * EX0 is enabled, which is only true when the bus is idle (not TX, not
 * mid-RX).  Phase-aligns Timer 1 to sample mid-bit. */
void int0_isr(void) __interrupt(0) {
    if (uart_tx_state == 0 && !uart_rx_active) {
        EX0 = 0;                       /* mute edge triggers for this frame */
        uart_rx_shift = 0;
        uart_rx_bit   = 0;
        uart_rx_active = 1;
        TL1 = T1_RELOAD_1P5_LO;        /* next fire = middle of bit 0 */
        TH1 = T1_RELOAD_1P5_HI;
        TF1 = 0;                       /* drop any pending overflow */
    }
}

void timer1_isr(void) __interrupt(3) {
    /* Reload first to minimize bit-period jitter (mode 1 = manual).
     * Always the 1-bit reload here; the special 1.5-bit reload is set
     * once by int0_isr and consumed as the interval that just elapsed. */
    TL1 = T1_RELOAD_LO;
    TH1 = T1_RELOAD_HI;

    if (uart_rx_active) {
        if (uart_rx_bit < 8) {
            if (BRIDGE_IO) uart_rx_shift |= (1 << uart_rx_bit);
            uart_rx_bit++;
        } else {
            /* stop-bit slot: commit the byte, re-arm start detection. */
            unsigned char next = (uart_rx_head + 1) & UART_RX_MASK;
            if (next != uart_rx_tail) {        /* drop on overflow */
                uart_rx_ring[uart_rx_head] = uart_rx_shift;
                uart_rx_head = next;
            }
            uart_rx_active = 0;
            IE0 = 0;                           /* clear latched edge */
            EX0 = 1;                           /* listen for next start */
        }
        return;
    }

    if (uart_tx_state == 0) {
        if (uart_tx_tail != uart_tx_head) {
            /* Begin a byte.  Mute RX for the whole TX burst. */
            if (!uart_tx_engaged) { EX0 = 0; uart_tx_engaged = 1; }
            uart_tx_shift = uart_tx_ring[uart_tx_tail];
            uart_tx_tail = (uart_tx_tail + 1) & UART_TX_MASK;
            BRIDGE_IO = 0;            /* start bit (drive LOW) */
            uart_tx_state = 1;
        } else if (uart_tx_engaged) {
            /* Burst fully drained -> hand the bus back to RX listen. */
            uart_tx_engaged = 0;
            IE0 = 0;
            EX0 = 1;
        }
        /* else truly idle: leave EX0 alone (avoid racing a latched edge). */
    } else if (uart_tx_state <= 8) {
        BRIDGE_IO = (uart_tx_shift & 1);   /* data bit, LSB first */
        uart_tx_shift >>= 1;
        uart_tx_state++;
    } else {
        /* state == 9: stop bit (release HIGH for one bit time). */
        BRIDGE_IO = 1;
        uart_tx_state = 0;
    }
}

static unsigned char uart_rx_avail(void) {
    return uart_rx_head != uart_rx_tail;
}

static unsigned char uart_rx_get(void) {
    unsigned char b = uart_rx_ring[uart_rx_tail];
    uart_rx_tail = (uart_rx_tail + 1) & UART_RX_MASK;
    return b;
}

/* ---- Framing + CRC8 (Phase 3-3) ----
 * Wire frame:  \xAA \x55  <payload ASCII>  <crcHi> <crcLo>  \n
 *   - payload: printable ASCII only, no '\n'
 *   - crcHi/crcLo: the CRC8 of the payload, as 2 uppercase hex chars
 *   - CRC8: poly 0x07, init 0x00, MSB-first, no reflection, no final
 *     XOR (CRC-8/SMBUS).  Computed over the payload bytes ONLY.
 * The ESPHome side MUST use this exact algorithm and CRC coverage.
 */
static unsigned char crc8_update(unsigned char crc, unsigned char b) {
    unsigned char i;
    crc ^= b;
    for (i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x07)
                           : (unsigned char)(crc << 1);
    return crc;
}

static char hex_digit(unsigned char nib) {
    nib &= 0x0F;
    return (nib < 10) ? (char)('0' + nib) : (char)('A' + (nib - 10));
}

/* TX primitives are defined later (with timer1_init); forward-declare
 * so the framing layer can use them here. */
static void uart_tx_byte(unsigned char b);

/* Emit one complete framed packet.  CRC accumulated during TX so we
 * never need strlen or a second pass over the payload. */
static void uart_send_frame(const char *payload) {
    const char *p = payload;
    unsigned char crc = 0x00;
    unsigned char ch;

    uart_tx_byte(0xAA);
    uart_tx_byte(0x55);
    while (*p) {
        ch = (unsigned char)*p++;
        uart_tx_byte(ch);
        crc = crc8_update(crc, ch);
    }
    uart_tx_byte((unsigned char)hex_digit(crc >> 4));
    uart_tx_byte((unsigned char)hex_digit(crc & 0x0F));
    uart_tx_byte('\n');
}

/* RX frame parser: a byte-fed state machine.  Discards everything
 * until a \xAA\x55 prefix (kills self-echo, ESP boot spam, partials),
 * accumulates printable payload to '\n', then verifies the trailing
 * 2 hex chars against CRC8 of the payload.  On success, exposes the
 * CRC-stripped, NUL-terminated payload + sets uart_frame_ready. */
#define UART_FRAME_MAX 56

static __xdata char uart_frame_buf[UART_FRAME_MAX + 1];
static unsigned char uart_frame_len = 0;
static unsigned char uart_frame_state = 0;  /* 0=AA,1=55,2=payload */
static volatile unsigned char uart_frame_ready = 0;

static unsigned char hex_val(char c) {
    if (c >= '0' && c <= '9') return (unsigned char)(c - '0');
    if (c >= 'A' && c <= 'F') return (unsigned char)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (unsigned char)(c - 'a' + 10);
    return 0xFF;
}

static void uart_frame_feed(unsigned char b) {
    switch (uart_frame_state) {
    case 0:
        if (b == 0xAA) uart_frame_state = 1;
        break;
    case 1:
        uart_frame_state = (b == 0x55) ? 2 : (b == 0xAA ? 1 : 0);
        uart_frame_len = 0;
        break;
    default:  /* 2: accumulating payload (incl. trailing 2 CRC chars) */
        if (b == '\n') {
            /* Need >= 2 chars (the CRC) plus a non-empty payload. */
            if (uart_frame_len >= 3) {
                unsigned char hi = hex_val(uart_frame_buf[uart_frame_len - 2]);
                unsigned char lo = hex_val(uart_frame_buf[uart_frame_len - 1]);
                if (hi != 0xFF && lo != 0xFF) {
                    unsigned char want = (unsigned char)((hi << 4) | lo);
                    unsigned char crc = 0x00;
                    unsigned char i;
                    for (i = 0; i < uart_frame_len - 2; i++)
                        crc = crc8_update(crc, (unsigned char)uart_frame_buf[i]);
                    if (crc == want && !uart_frame_ready) {
                        uart_frame_buf[uart_frame_len - 2] = '\0'; /* strip CRC */
                        uart_frame_ready = 1;
                    }
                }
            }
            uart_frame_state = 0;
        } else if (b == 0xAA) {
            /* Resync: a new prefix mid-payload abandons this frame. */
            uart_frame_state = 1;
        } else if (b >= 0x20 && b <= 0x7E &&
                   uart_frame_len < UART_FRAME_MAX) {
            uart_frame_buf[uart_frame_len++] = (char)b;
        } else {
            /* Non-printable / overflow -> drop frame, rescan. */
            uart_frame_state = 0;
        }
        break;
    }
}

/* Drain the RX ring through the frame parser.  Call from the main
 * loop; latency-tolerant (the ESP polls every ~2 s). */
static void uart_poll_frames(void) {
    while (uart_rx_avail())
        uart_frame_feed(uart_rx_get());
}

static void timer1_init(void) {
    AUXR |= (1 << 6);                /* T1 in 1T mode */
    TMOD = (TMOD & 0x0F) | 0x10;     /* T1 mode 1 (16-bit), preserve T0 */
    TL1 = T1_RELOAD_LO;
    TH1 = T1_RELOAD_HI;
    ET1 = 1;                         /* enable Timer 1 interrupt */
    TR1 = 1;                         /* run Timer 1 (EA already set) */
}

/* Enqueue one byte.  Busy-waits if the ring is full (the ISR is
 * always draining, so this can't deadlock once timer1_init ran). */
static void uart_tx_byte(unsigned char b) {
    unsigned char next = (uart_tx_head + 1) & UART_TX_MASK;
    while (next == uart_tx_tail) { }   /* ring full -- wait for ISR */
    uart_tx_ring[uart_tx_head] = b;
    uart_tx_head = next;
}

static void uart_tx_str(const char *s) {
    while (*s) uart_tx_byte((unsigned char)*s++);
}

/* Master HA-bridge enable.  Set to 0 to fully disable the soft-UART
 * (no Timer 1, no INT0, no uart_service) -- A/B test for whether the
 * bridge is what's disrupting buttons/jog. */
#define P3_UART_ENABLE  1   /* HA bridge active */

/* Button diagnostic: replaces the entire main loop with a minimal
 * read-button -> show-on-LCD loop (no position/duty/storm/UART).
 * Isolates whether the raw button ADC is reliable at all.  Set 0 for
 * normal firmware. */
#define P3_BTN_DEBUG  0

/* P3-1 validation: periodic TX self-test.  Done -- left at 0. */
#define P3_UART_TX_TEST  0

/* P3-2 validation: byte echo.  Done -- left at 0. */
#define P3_UART_RX_ECHO  0

/* P3-3 validation: fixed framed packet.  Done -- left at 0. */
#define P3_UART_FRAME_TEST  0

/* P3-4 validation: periodic broadcast.  Done -- ESP polls trigger
 * replies in P3-5.  Left at 0; flip to 1 if debugging the wire without
 * a working ESP poller. */
#define P3_UART_STATUS_TEST  0

/* ---- IAP / EEPROM ---- */
#define IAP_ENABLE_22MHZ  0x83
#define IAP_CMD_READ      0x01
#define IAP_CMD_PROGRAM   0x02
#define IAP_CMD_ERASE     0x03

#define EEPROM_BASE       0x0000
#define CONFIG_MAGIC_0    0xC0
#define CONFIG_MAGIC_1    0xDE

static void iap_idle(void) {
    IAP_CONTR = 0;
    IAP_CMD = 0;
    IAP_TRIG = 0;
    IAP_ADDRH = 0xFF;
    IAP_ADDRL = 0xFF;
}

static unsigned char iap_read_byte(unsigned int addr) {
    unsigned char dat;
    EA = 0;
    IAP_CONTR = IAP_ENABLE_22MHZ;
    IAP_CMD = IAP_CMD_READ;
    IAP_ADDRH = addr >> 8;
    IAP_ADDRL = addr & 0xFF;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    __asm
        nop
    __endasm;
    dat = IAP_DATA;
    iap_idle();
    EA = 1;
    return dat;
}

static void iap_program_byte(unsigned int addr, unsigned char dat) {
    EA = 0;
    IAP_CONTR = IAP_ENABLE_22MHZ;
    IAP_CMD = IAP_CMD_PROGRAM;
    IAP_ADDRH = addr >> 8;
    IAP_ADDRL = addr & 0xFF;
    IAP_DATA = dat;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    __asm
        nop
    __endasm;
    iap_idle();
    EA = 1;
}

static void iap_erase_sector(unsigned int addr) {
    EA = 0;
    IAP_CONTR = IAP_ENABLE_22MHZ;
    IAP_CMD = IAP_CMD_ERASE;
    IAP_ADDRH = addr >> 8;
    IAP_ADDRL = addr & 0xFF;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    __asm
        nop
    __endasm;
    iap_idle();
    EA = 1;
}

static unsigned char config_is_valid(void) {
    return (iap_read_byte(EEPROM_BASE + 0) == CONFIG_MAGIC_0 &&
            iap_read_byte(EEPROM_BASE + 1) == CONFIG_MAGIC_1);
}

/* Cached validity.  config_is_valid() does 2 IAP/flash reads with
 * interrupts disabled -- calling it every main-loop iteration (as
 * storm_check did) is wasteful and adds EA=0 windows.  The EEPROM
 * only changes on config_save(), so cache it and refresh there. */
static unsigned char cfg_valid = 0;
static void config_valid_refresh(void) { cfg_valid = config_is_valid(); }

/* Config struct layout in EEPROM (little-endian 16-bit):
 *   0..1  : magic (0xC0 0xDE)
 *   2..3  : ns_stroke_ms (lo, hi)
 *   4..5  : ew_stroke_ms (lo, hi)
 *   6     : horiz_ns_pct (0..100; default 50)
 *   7     : horiz_ew_pct (0..100; default 50)
 *   8     : wind_storm_mps -- enter storm park above this wind speed
 *   9     : wind_release_mps -- release storm dwell below this speed
 *   10    : storm_dwell_min -- minutes wind must stay below release
 *   11    : track_thresh -- sun differential ADC counts to trigger a move
 *   12    : role (1=primary, 2=secondary)
 *   13    : wind_source (0=local, 1=remote)
 */
static unsigned int ns_stroke_ms = 0;
static unsigned int ew_stroke_ms = 0;
static unsigned char horiz_ns_pct = 50;
static unsigned char horiz_ew_pct = 50;
static unsigned char wind_storm_mps   = 15;
static unsigned char wind_release_mps = 10;
static unsigned char storm_dwell_min  = 10;
static unsigned char track_thresh     = 3;
static unsigned char role           = 2;   /* 1=primary, 2=secondary */
static unsigned char wind_source    = 0;   /* 0=local, 1=remote */

/* Setting range bounds.  Used by ST_SETTINGS_EDIT for clamping and
 * by config_load() for sanity-checking unprogrammed EEPROM bytes. */
#define WIND_STORM_MIN    5
#define WIND_STORM_MAX   30
#define WIND_RELEASE_MIN  0
#define WIND_RELEASE_MAX 20
#define DWELL_MIN_MIN     1
#define DWELL_MIN_MAX    60
#define TRACK_THRESH_MIN  1
#define TRACK_THRESH_MAX 99
#define ROLE_MIN          1
#define ROLE_MAX          2
#define WIND_SOURCE_MIN   0
#define WIND_SOURCE_MAX   1

/* Mesh config-protocol field IDs (see P4-6).  RW fields are operator
 * tunables; RO fields are calibration/install data that should only
 * be changed from the local UI. */
#define CFG_F_WIND_STORM    0x01
#define CFG_F_WIND_RELEASE  0x02
#define CFG_F_STORM_DWELL   0x03
#define CFG_F_TRACK_THRESH  0x04
#define CFG_F_NS_STROKE     0x10
#define CFG_F_EW_STROKE     0x11
#define CFG_F_HORIZ_NS      0x12
#define CFG_F_HORIZ_EW      0x13
#define CFG_F_ROLE          0x20
#define CFG_F_WIND_SOURCE   0x21

#define CFG_STATUS_OK       0
#define CFG_STATUS_RO       1
#define CFG_STATUS_RANGE    2
#define CFG_STATUS_UNKNOWN  3

static unsigned int iap_read_u16(unsigned int addr) {
    unsigned int lo = iap_read_byte(addr);
    unsigned int hi = iap_read_byte(addr + 1);
    return (hi << 8) | lo;
}

static void config_load(void) {
    if (!config_is_valid()) return;
    ns_stroke_ms = iap_read_u16(EEPROM_BASE + 2);
    ew_stroke_ms = iap_read_u16(EEPROM_BASE + 4);
    horiz_ns_pct = iap_read_byte(EEPROM_BASE + 6);
    horiz_ew_pct = iap_read_byte(EEPROM_BASE + 7);
    wind_storm_mps   = iap_read_byte(EEPROM_BASE + 8);
    wind_release_mps = iap_read_byte(EEPROM_BASE + 9);
    storm_dwell_min  = iap_read_byte(EEPROM_BASE + 10);
    track_thresh     = iap_read_byte(EEPROM_BASE + 11);
    role        = iap_read_byte(EEPROM_BASE + 12);
    wind_source = iap_read_byte(EEPROM_BASE + 13);
    if (horiz_ns_pct > 100) horiz_ns_pct = 50;
    if (horiz_ew_pct > 100) horiz_ew_pct = 50;
    /* Reject unprogrammed (0xFF) and out-of-range; revert to defaults. */
    if (wind_storm_mps < WIND_STORM_MIN || wind_storm_mps > WIND_STORM_MAX)
        wind_storm_mps = 15;
    if (wind_release_mps > WIND_RELEASE_MAX)
        wind_release_mps = 10;
    if (storm_dwell_min < DWELL_MIN_MIN || storm_dwell_min > DWELL_MIN_MAX)
        storm_dwell_min = 10;
    if (track_thresh < TRACK_THRESH_MIN || track_thresh > TRACK_THRESH_MAX)
        track_thresh = 3;
    if (role < ROLE_MIN || role > ROLE_MAX) role = 2;             /* default secondary */
    if (wind_source > WIND_SOURCE_MAX) wind_source = 0;           /* default local */
    /* Invariant: release threshold must be strictly less than storm threshold. */
    if (wind_release_mps >= wind_storm_mps)
        wind_release_mps = (wind_storm_mps > 0) ? wind_storm_mps - 1 : 0;
}

static void config_save(void) {
    /* Re-apply invariant before persisting -- catches the case where
     * the user just edited release up past storm via the menu. */
    if (wind_release_mps >= wind_storm_mps)
        wind_release_mps = (wind_storm_mps > 0) ? wind_storm_mps - 1 : 0;

    iap_erase_sector(EEPROM_BASE);
    iap_program_byte(EEPROM_BASE + 0, CONFIG_MAGIC_0);
    iap_program_byte(EEPROM_BASE + 1, CONFIG_MAGIC_1);
    iap_program_byte(EEPROM_BASE + 2, ns_stroke_ms & 0xFF);
    iap_program_byte(EEPROM_BASE + 3, ns_stroke_ms >> 8);
    iap_program_byte(EEPROM_BASE + 4, ew_stroke_ms & 0xFF);
    iap_program_byte(EEPROM_BASE + 5, ew_stroke_ms >> 8);
    iap_program_byte(EEPROM_BASE + 6, horiz_ns_pct);
    iap_program_byte(EEPROM_BASE + 7, horiz_ew_pct);
    iap_program_byte(EEPROM_BASE + 8, wind_storm_mps);
    iap_program_byte(EEPROM_BASE + 9, wind_release_mps);
    iap_program_byte(EEPROM_BASE + 10, storm_dwell_min);
    iap_program_byte(EEPROM_BASE + 11, track_thresh);
    iap_program_byte(EEPROM_BASE + 12, role);
    iap_program_byte(EEPROM_BASE + 13, wind_source);
    config_valid_refresh();   /* config now persisted -> cache fresh */
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

/* Average `samples` ADC reads; pushes noise floor down by sqrt(samples). */
static unsigned int adc_read_avg(unsigned char channel, unsigned char samples) {
    unsigned long sum = 0;
    unsigned char i;
    for (i = 0; i < samples; i++) {
        sum += adc_read(channel);
    }
    return (unsigned int)(sum / samples);
}

/* Settled read for high-impedance sources (the button resistor ladder
 * is up to ~120 kOhm).  The first conversion after a channel mux switch
 * is contaminated by S/H-cap charge left from the previous channel; we
 * discard it (its conversion time lets the cap settle through the new
 * source) and return the second.  Averaging the settled samples further
 * rejects noise so the debounce can latch on a short press. */
static unsigned int adc_read_settled(unsigned char channel,
                                     unsigned char samples) {
    adc_read(channel);                 /* discard 1: absorbs mux switch */
    adc_read(channel);                 /* discard 2: high-Z ladder settle */
    return adc_read_avg(channel, samples);
}

static unsigned char wind_mps(unsigned int adc) {
    unsigned long v = (unsigned long)adc * 125UL / 1023UL;
    return (v > 99) ? 99 : (unsigned char)v;
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

/* ---- Axis state machine (carries over from Phase 2A) ----
 * Physical H-bridge pairs:
 *   N/S axis (tilt):  RELAY_N (FWD/extend) + RELAY_W (REV/retract)
 *   E/W axis (rotate): RELAY_S (FWD/extend) + RELAY_E (REV/retract)
 * `set_axis_*()` enforce mutex with a 10 ms release delay on reversal.
 */
typedef enum {
    AXIS_OFF = 0,
    AXIS_FWD,
    AXIS_REV
} axis_state_t;

static axis_state_t ns_state = AXIS_OFF;
static axis_state_t ew_state = AXIS_OFF;

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

/* ---- Position tracking ----
 * pos_ms is the integrated "FWD time minus REV time since last zero",
 * saturated to [0, stroke_ms].  0 = retract endstop, stroke_ms = extend.
 *
 * Backlash compensation: the position integrator supports per-axis
 * "skip first N ms of FWD after a REV" via `backlash_ms`.  Set to 0 on
 * this build -- the backlash test (Menu -> Backlash) showed gear slack
 * below 20 ms, which is negligible for solar tracking accuracy and
 * gets absorbed by the saturation clamp at stroke endpoints.  If a
 * future actuator has measurable backlash, raise this constant.
 */
#define EXTEND_BACKLASH_MS  0U

/* __xdata to keep these file-scope statics out of the DSEG overlay
 * pool -- 14 bytes there matters at the 128-byte budget limit. */
static __xdata unsigned int ns_pos_ms = 0;
static __xdata unsigned int ew_pos_ms = 0;
static __xdata unsigned int ns_backlash_ms = EXTEND_BACKLASH_MS;
static __xdata unsigned int ew_backlash_ms = EXTEND_BACKLASH_MS;
static __xdata axis_state_t ns_pos_prev = AXIS_OFF;
static __xdata axis_state_t ew_pos_prev = AXIS_OFF;
static __xdata unsigned long pos_last_tick_ms = 0;

/* ---- Duty cycle (motor thermal protection) ----
 * Per axis: accumulate motor-on time.  At DUTY_ON_LIMIT_MS, force the
 * axis off and lock it out for DUTY_OFF_LOCKOUT_MS, then reset.
 * lockout_end == 0 means "not locked" (a real end-time is always
 * now + 18min, never 0).  Runs every main-loop iteration; does NOT
 * run during blocking cal/boot-zero (main loop isn't iterating then),
 * which is intended -- a single cal stroke is well under the limit. */
#define DUTY_ON_LIMIT_MS     120000UL   /* 2 min max continuous-ish on */
#define DUTY_OFF_LOCKOUT_MS  1080000UL  /* then 18 min lockout */

static __xdata unsigned long ns_duty_on_ms = 0;
static __xdata unsigned long ew_duty_on_ms = 0;
static __xdata unsigned long ns_duty_lockout_end = 0;
static __xdata unsigned long ew_duty_lockout_end = 0;
static __xdata unsigned long duty_last_tick_ms = 0;

/* Wind cache: storm_check() samples wind at 1 Hz; the cached m/s value
 * is what UI screens (jog, idle) display.  This is the architectural
 * decoupling that fixed jog -- screens never read ADC themselves, so
 * the button channel never gets contaminated by per-loop mux switches. */
static __xdata unsigned char wind_mps_cached = 0;

static __xdata unsigned char remote_wind_mps = 0;
static __xdata unsigned long remote_wind_last_update_ms = 0;
#define REMOTE_WIND_TIMEOUT_MS 20000UL   /* Q5 failsafe: 4 missed 5s broadcasts */

/* ---- Storm interlock (Phase 2C-9) ----
 * High wind forces ST_STORM: drive to the saved horizontal position
 * (PARKING), then hold there (HOLDING) until wind has stayed below
 * the release threshold continuously for storm_dwell_min minutes.
 * storm_parking also suspends duty enforcement -- getting the array
 * flat in a gust outranks motor thermal protection. */
#define STORM_POS_TOL_MS  500U   /* "close enough" to horizontal target */

typedef enum { STORM_PARKING = 0, STORM_HOLDING } storm_phase_t;
static __xdata storm_phase_t storm_phase = STORM_PARKING;
static __xdata unsigned long storm_dwell_start_ms = 0;
static __xdata unsigned char storm_parking = 0;

/* Operator-forced storm (via HA / mesh).  Sticky: set by !park,
 * cleared only by !release.  Blocks dwell exit while set. */
static __xdata unsigned char storm_forced = 0;

/* Auto-managed wind failsafe (only when wind_source==1, i.e. remote).
 * Set by storm_check() when remote-wind broadcasts have been silent
 * for >= REMOTE_WIND_TIMEOUT_MS; cleared automatically as soon as a
 * fresh broadcast arrives.  Also triggers storm entry like storm_forced,
 * but does NOT block dwell exit -- so once broadcasts resume, the
 * normal dwell-then-exit path runs. */
static __xdata unsigned char wind_failsafe = 0;

static unsigned char ns_duty_locked(void) { return ns_duty_lockout_end != 0; }
static unsigned char ew_duty_locked(void) { return ew_duty_lockout_end != 0; }

static void duty_tick(void) {
    static __xdata unsigned long now, dt;
    now = millis();
    dt = now - duty_last_tick_ms;
    if (dt == 0) return;
    duty_last_tick_ms = now;

    /* Storm park bypasses duty: reaching horizontal in a gust is more
     * important than motor thermal limits.  Timestamp still advanced
     * above so post-park dt isn't a huge jump. */
    if (storm_parking) return;

    /* Self-heal: a lockout_end more than DUTY_OFF_LOCKOUT_MS in the
     * future is impossible to set legitimately (it's always now+limit).
     * Such a value can only come from corruption / a bad init and would
     * otherwise never satisfy `now >= lockout_end` -> permanent lock.
     * Force-clear it so the interlock can never brick the actuators. */
    if (ns_duty_lockout_end != 0 &&
        ns_duty_lockout_end - now > DUTY_OFF_LOCKOUT_MS) {
        ns_duty_lockout_end = 0;
        ns_duty_on_ms = 0;
    }
    if (ew_duty_lockout_end != 0 &&
        ew_duty_lockout_end - now > DUTY_OFF_LOCKOUT_MS) {
        ew_duty_lockout_end = 0;
        ew_duty_on_ms = 0;
    }

    if (ns_duty_lockout_end != 0) {
        if (now >= ns_duty_lockout_end) {
            ns_duty_lockout_end = 0;
            ns_duty_on_ms = 0;
        }
    } else if (ns_state != AXIS_OFF) {
        ns_duty_on_ms += dt;
        if (ns_duty_on_ms >= DUTY_ON_LIMIT_MS) {
            set_axis_ns(AXIS_OFF);
            ns_duty_lockout_end = now + DUTY_OFF_LOCKOUT_MS;
        }
    }

    if (ew_duty_lockout_end != 0) {
        if (now >= ew_duty_lockout_end) {
            ew_duty_lockout_end = 0;
            ew_duty_on_ms = 0;
        }
    } else if (ew_state != AXIS_OFF) {
        ew_duty_on_ms += dt;
        if (ew_duty_on_ms >= DUTY_ON_LIMIT_MS) {
            set_axis_ew(AXIS_OFF);
            ew_duty_lockout_end = now + DUTY_OFF_LOCKOUT_MS;
        }
    }
}

/* Reset position to 0 and prime backlash.  Call after any operation
 * that lands the actuator at the retract endstop (boot zero, cal).
 * Re-arms BOTH tick clocks so the blocking-call time gap isn't
 * miscredited.  Does NOT touch duty thermal state -- that's a
 * separate policy decision (cold vs. hot), see duty_cold_reset()
 * and duty_force_cooldown() below. */
static void position_reset(void) {
    ns_pos_ms = 0;
    ew_pos_ms = 0;
    ns_backlash_ms = EXTEND_BACKLASH_MS;
    ew_backlash_ms = EXTEND_BACKLASH_MS;
    ns_pos_prev = AXIS_OFF;
    ew_pos_prev = AXIS_OFF;
    pos_last_tick_ms = millis();
    duty_last_tick_ms = millis();
}

/* COLD policy: motor assumed cool (long power-off before boot).  Zero
 * the interlock.  Imperative, not relying on static __xdata init --
 * a stuck-true duty lockout at boot bricks the actuators and is
 * unrecoverable, so it must be cleared in code, unconditionally. */
static void duty_cold_reset(void) {
    ns_duty_on_ms = 0;
    ew_duty_on_ms = 0;
    ns_duty_lockout_end = 0;
    ew_duty_lockout_end = 0;
    duty_last_tick_ms = millis();
}

/* HOT policy: calibration just ran each axis ~3x stroke (~3 min for a
 * 65 s stroke) -- well past the 2 min rating -- but the blocking cal
 * couldn't be observed by duty_tick().  Impute that thermal cost:
 * force both axes into the full cooldown lockout so the motors rest
 * (padlock shows).  A user who must move sooner can power-cycle (a
 * deliberate cold reset -- they're at the controller anyway). */
static void duty_force_cooldown(void) {
    unsigned long now = millis();
    ns_duty_on_ms = 0;
    ew_duty_on_ms = 0;
    ns_duty_lockout_end = now + DUTY_OFF_LOCKOUT_MS;
    ew_duty_lockout_end = now + DUTY_OFF_LOCKOUT_MS;
    duty_last_tick_ms = now;
}

/* Integrate one axis's contribution this tick.  Encapsulates the
 * FWD-with-backlash and REV logic so the per-axis call sites are
 * symmetric. */
static unsigned int axis_pos_step(axis_state_t state, axis_state_t prev,
                                  unsigned int pos_ms,
                                  unsigned int stroke_ms,
                                  unsigned int *backlash_ms,
                                  unsigned long dt) {
    /* `static __xdata` keeps the long arithmetic temps out of DSEG --
     * same overlay-budget workaround as cal_wait_stall. */
    static __xdata unsigned long active, eat, np;

    /* Entering REV from anything: prime backlash for the next FWD. */
    if (state == AXIS_REV && prev != AXIS_REV) {
        *backlash_ms = EXTEND_BACKLASH_MS;
    }

    if (state == AXIS_FWD) {
        active = dt;
        if (*backlash_ms > 0) {
            eat = (active < *backlash_ms) ? active : *backlash_ms;
            *backlash_ms -= (unsigned int)eat;
            active -= eat;
        }
        if (active > 0) {
            np = (unsigned long)pos_ms + active;
            pos_ms = (np > stroke_ms) ? stroke_ms : (unsigned int)np;
        }
    } else if (state == AXIS_REV) {
        if (dt >= (unsigned long)pos_ms) pos_ms = 0;
        else pos_ms -= (unsigned int)dt;
    }
    return pos_ms;
}

/* Update position estimates based on elapsed time since last call.
 * Should run once per main-loop iteration BEFORE any code that might
 * change axis state, so the dt that elapses gets credited to the
 * state that was actually in effect. */
static void position_tick(void) {
    static __xdata unsigned long now, dt;
    now = millis();
    dt = now - pos_last_tick_ms;
    if (dt == 0) return;
    pos_last_tick_ms = now;

    ns_pos_ms = axis_pos_step(ns_state, ns_pos_prev, ns_pos_ms,
                              ns_stroke_ms, &ns_backlash_ms, dt);
    ew_pos_ms = axis_pos_step(ew_state, ew_pos_prev, ew_pos_ms,
                              ew_stroke_ms, &ew_backlash_ms, dt);
    ns_pos_prev = ns_state;
    ew_pos_prev = ew_state;
}

/* Position as 0..100 percent of stroke.  Returns 0 if stroke not yet
 * calibrated (avoid divide-by-zero). */
static unsigned char pos_to_pct(unsigned int pos_ms, unsigned int stroke_ms) {
    unsigned long pct;
    if (stroke_ms == 0) return 0;
    pct = (unsigned long)pos_ms * 100UL / stroke_ms;
    return (pct > 100) ? 100 : (unsigned char)pct;
}

/* Print a signed value as sign + 3 zero-padded digits, clamped 999. */
static void lcd_print_sint3(int v) {
    unsigned int a;
    if (v < 0) { lcd_putc('-'); a = (unsigned int)(-v); }
    else       { lcd_putc('+'); a = (unsigned int)v;    }
    if (a > 999) a = 999;
    lcd_putc('0' + (a / 100));
    lcd_putc('0' + ((a / 10) % 10));
    lcd_putc('0' + (a % 10));
}

/* ---- Auto-tracking (Phase 2C-8) ----
 * Differential-balance pulse-tracker.  Every TRACK_CHECK_PERIOD_MS,
 * read the four sun sensors; if an axis's differential exceeds the
 * threshold (and the axis is idle and not duty-locked), pulse it
 * TRACK_PULSE_MS in the correction direction.  Pulses are damped by
 * the check period so transient shadows don't cause overshoot. */
#define TRACK_CHECK_PERIOD_MS  5000UL
#define TRACK_PULSE_MS         500U
#define TRACK_SENSOR_AVG       8     /* samples per sun sensor read */
/* Move threshold is the configurable `track_thresh` setting (ADC
 * counts).  Sensor signal scales with illuminance, so the right value
 * is wildly different indoors vs. real sun -- hence a Setting. */

static __xdata int           track_dN = 0;   /* cached for display */
static __xdata int           track_dE = 0;
static __xdata unsigned long track_last_check_ms = 0;
static __xdata unsigned long ns_pulse_end_ms = 0;
static __xdata unsigned long ew_pulse_end_ms = 0;
static __xdata unsigned char goto_az_target_pct = 0;
static __xdata unsigned char goto_el_target_pct = 0;
static __xdata unsigned char goto_active = 0;
/* track_tick() is defined after the state_t enum (it mutates state). */

/* ---- Calibration ----
 * Runs as a blocking sub-routine called from the ST_CAL main-loop case.
 * Polls QUIT at every yield point so the user can abort.  All four phases
 * (retract+extend per axis) use the same three-band classifier on
 * dI = current_idle - stall_adc:
 *
 *     dI band     state       meaning
 *     ----------- ----------- --------------------------------------------
 *     dI >= 5     MOVING      motor drawing load current, panel in motion
 *     dI in 2..4  STALLED     residual draw or motor-just-stopped; band
 *                             tightened from [1..4] to absorb the ~2-count
 *                             ADC noise floor that caused idle flicker
 *     dI <= 1     IDLE        no motor current (or below noise floor)
 *
 * Physical interpretation: the pin 8 network is a soft-Zener-clamped
 * rail-droop monitor.  Transfer function bus -> pin ~= Z_z / R1 ~=
 * 77 / 2700 ~= 2.85%.  On the install (battery + lossy wiring),
 * 1-3 V of motor-induced bus droop translates to 6-17 ADC counts at
 * the pin -- comfortably above the noise floor.  On a stiff bench
 * supply, droop is smaller and the signal degrades to marginal.
 *
 * A stroke is accepted as stalled only after MOVING has been entered
 * first -- prevents the relay-on inrush from looking like a stall on
 * stroke 1.  Also: the actuators have internal limit switches that
 * mechanically open the motor circuit at the endstop, so even if
 * stall detection misfires, the hardware self-protects.
 *
 * Timing:
 *   - Skip the first 500 ms after relay-on (relay coil pickup ~10 ms +
 *     motor inrush + DC supply ringdown).
 *   - 200 ms continuous in the STALLED band (~4 consecutive 50 ms
 *     samples) confirms stall.  De-assert relay and proceed.
 *   - Hard timeout at 120 s -- something's wrong if we hit it (relay
 *     not switching, wire off, supply at current limit, etc).
 */
#define CAL_STARTUP_MS     500
#define CAL_STALL_COUNT    4       /* * 50 ms = 200 ms confirmation */
#define CAL_TIMEOUT_MS     120000UL
#define CAL_INTER_STEP_MS  1000

typedef enum {
    CAL_OK = 0,
    CAL_ABORTED,
    CAL_TIMEOUT
} cal_result_t;

static unsigned int current_idle = 0;

/* Print "+NN" or "-NN" representing (adc - baseline), 2 digits clamped. */
static void lcd_print_dI(unsigned int adc, unsigned int baseline) {
    unsigned int diff;
    if (adc >= baseline) {
        lcd_putc('+');
        diff = adc - baseline;
    } else {
        lcd_putc('-');
        diff = baseline - adc;
    }
    if (diff > 99) diff = 99;
    lcd_putc('0' + (diff / 10));
    lcd_putc('0' + (diff % 10));
}

static void lcd_print_secs_tenths(unsigned long ms) {
    unsigned int sec = (unsigned int)(ms / 1000);
    unsigned int tenths = (unsigned int)((ms / 100) % 10);
    if (sec > 99) sec = 99;
    lcd_putc('0' + (sec / 10));
    lcd_putc('0' + (sec % 10));
    lcd_putc('.');
    lcd_putc('0' + tenths);
    lcd_putc('s');
}

static void cal_show(const char *label, unsigned long elapsed) {
    lcd_goto(0, 0); lcd_print_padded(label, 16);
    lcd_goto(1, 0); lcd_print("Time      ");
    lcd_print_secs_tenths(elapsed);
    lcd_putc(' ');
}

/* Edge-detect QUIT for use inside the blocking calibrate(). */
static unsigned char cal_quit_pressed(button_t *prev) {
    unsigned int adc = adc_read_settled(ADC_CH_BUTTONS, 4);
    button_t curr = button_classify(adc);
    unsigned char quit = (curr == BTN_QUIT && *prev == BTN_NONE);
    *prev = curr;
    return quit;
}

/* Three-band classification of the stall-current signal for the jog
 * display.  Matches the thresholds used in cal_wait_stall(): */
typedef enum {
    DI_STALLED = 0,   /* dI > -3   (no significant droop) */
    DI_TRANSIENT,     /* dI in [-5, -3]  (between motion and stall) */
    DI_MOVING         /* dI < -5   (motor pulling load current) */
} di_band_t;

static di_band_t di_classify(unsigned int stall_adc) {
    int di = (int)stall_adc - (int)current_idle;
    if (di < -5) return DI_MOVING;
    if (di < -3) return DI_TRANSIENT;
    return DI_STALLED;
}

/* Drive one direction (axis already set by caller) and wait for stall.
 * Returns elapsed ms via *elapsed_out.  Caller is responsible for
 * de-asserting the relay (we don't know which axis to stop).
 *
 * Uses signed dI (stall_adc - current_idle) directly:
 *   - motion:  dI < -5  (motor pulling load current, droop visible)
 *   - stall:   dI > -3  (no significant droop; includes overshoot)
 *   - between: ambiguous transient, hold state
 *
 * Asymmetric band edges (motion at -5, stall at -3) give a 2-LSB dead
 * zone in the middle so noisy samples bouncing between motion and
 * stall don't ping-pong the state machine.  saw_motion ensures we
 * don't false-trigger on the inrush-quiet window before motion. */
static cal_result_t cal_wait_stall(const char *label,
                                   unsigned long *elapsed_out,
                                   button_t *prev) {
    /* `static __xdata` to keep these out of the DSEG overlay region --
     * SDCC's small-model overlay budget is only 128 bytes and the cal
     * call chain (5+ levels) blows it.  XRAM has 2 KB free. */
    static __xdata unsigned long start, elapsed, t;
    unsigned char stall_count = 0;
    unsigned char saw_motion = 0;

    /* Skip relay-on inrush transient. */
    {
        t = millis();
        while (millis() - t < CAL_STARTUP_MS) {
            if (cal_quit_pressed(prev)) return CAL_ABORTED;
            delay_ms(20);
        }
    }

    start = millis();
    for (;;) {
        int di_signed;

        if (cal_quit_pressed(prev)) return CAL_ABORTED;

        di_signed = (int)adc_read_avg(ADC_CH_STALL, 8) - (int)current_idle;

        if (di_signed < -5) {
            /* Motion: clear progress, latch saw_motion. */
            saw_motion = 1;
            stall_count = 0;
        } else if (di_signed > -3 && saw_motion) {
            /* Stall (dI > -3) after motion observed. */
            if (++stall_count >= CAL_STALL_COUNT) {
                *elapsed_out = millis() - start;
                return CAL_OK;
            }
        } else {
            /* dI in [-5, -3]: transient between motion and stall.
             * Don't increment, don't reset -- just hold. */
        }

        elapsed = millis() - start;
        if (elapsed > CAL_TIMEOUT_MS) return CAL_TIMEOUT;

        cal_show(label, elapsed);
        delay_ms(50);
    }
}

static void cal_inter_step_wait(button_t *prev) {
    /* 1 s settle between steps (relay release + actuator wind-down). */
    static __xdata unsigned long t;
    t = millis();
    while (millis() - t < CAL_INTER_STEP_MS) {
        if (cal_quit_pressed(prev)) return;     /* swallow; main loop will catch on next QUIT */
        delay_ms(50);
    }
}

/* ---- Cal axis dispatch + zero/measure helpers ---- */

typedef enum {
    CAL_AXIS_NS = 0,
    CAL_AXIS_EW = 1
} cal_axis_t;

static void cal_axis_set(cal_axis_t axis, axis_state_t state) {
    if (axis == CAL_AXIS_NS) set_axis_ns(state);
    else                     set_axis_ew(state);
}

#define CAL_BUMP_OFF_MS              1000
#define CAL_EXTEND_STALL_DELAY_MS    1500UL  /* dI takes ~1.5s to settle into the stall band */
                                             /* after extend motor actually stops at endstop. */
                                             /* Subtract from t_ext to get true motion time.  */

/* Drive an axis to the retract endstop and stop.  First "bumps off"
 * any pre-existing retract-endstop position with a short extend stroke,
 * then retracts to stall.  The bump-off guarantees the subsequent
 * retract sees motion (avoids the saw_motion gate hanging forever
 * when starting at the endstop).  Used at both boot (auto-zero) and
 * the start of each axis measurement during cal. */
static cal_result_t cal_zero_axis(cal_axis_t axis,
                                  const char *label,
                                  button_t *prev) {
    static __xdata unsigned long elapsed_dummy, t;
    cal_result_t r;

    /* Bump-off: drive extend for a fixed short duration. */
    cal_axis_set(axis, AXIS_FWD);
    {
        t = millis();
        while (millis() - t < CAL_BUMP_OFF_MS) {
            if (cal_quit_pressed(prev)) {
                cal_axis_set(axis, AXIS_OFF);
                return CAL_ABORTED;
            }
            cal_show(label, millis() - t);
            delay_ms(50);
        }
    }
    cal_axis_set(axis, AXIS_OFF);
    cal_inter_step_wait(prev);

    /* Retract to stall.  Axis now at retract endstop = position 0. */
    cal_axis_set(axis, AXIS_REV);
    r = cal_wait_stall(label, &elapsed_dummy, prev);
    cal_axis_set(axis, AXIS_OFF);
    return r;
}

/* Measure stroke time for one axis.  Sequence:
 *   1. zero (bump-off + retract-to-stall)
 *   2. extend to stall, time it -> t_ext  (corrected for backlash)
 *   3. retract to stall, time it -> t_ret
 * Returns max(t_ext, t_ret) as the safe stroke time.
 *
 * Extend stall-delay correction: at the extend endstop, the dI signal
 * takes ~1.5 s to settle into the "no current" band after the motor
 * actually stops (slow rail recovery + filter cap discharge through
 * the 2.7k/3k network).  cal_wait_stall therefore overshoots the real
 * stroke time by that amount.  Subtract CAL_EXTEND_STALL_DELAY_MS
 * from t_ext to get the true motion duration.  Retract has no
 * equivalent delay because the retract-endstop stall signature is
 * promptly visible. */
static cal_result_t cal_measure_axis(cal_axis_t axis,
                                     const char *lbl_zero,
                                     const char *lbl_ext,
                                     const char *lbl_ret,
                                     unsigned int *stroke_out,
                                     button_t *prev) {
    static __xdata unsigned long t_ext, t_ret, t_max;
    cal_result_t r;

    r = cal_zero_axis(axis, lbl_zero, prev);
    if (r != CAL_OK) return r;
    cal_inter_step_wait(prev);

    cal_axis_set(axis, AXIS_FWD);
    r = cal_wait_stall(lbl_ext, &t_ext, prev);
    cal_axis_set(axis, AXIS_OFF);
    if (r != CAL_OK) return r;
    /* Subtract extend stall-detect delay from t_ext.  Saturate to 0
     * to avoid unsigned underflow on a very short stroke. */
    t_ext = (t_ext > CAL_EXTEND_STALL_DELAY_MS) ? (t_ext - CAL_EXTEND_STALL_DELAY_MS) : 0;
    cal_inter_step_wait(prev);

    cal_axis_set(axis, AXIS_REV);
    r = cal_wait_stall(lbl_ret, &t_ret, prev);
    cal_axis_set(axis, AXIS_OFF);
    if (r != CAL_OK) return r;

    t_max = (t_ext > t_ret) ? t_ext : t_ret;
    *stroke_out = (t_max > 0xFFFF) ? 0xFFFF : (unsigned int)t_max;
    return CAL_OK;
}

static cal_result_t calibrate(void) {
    button_t prev = BTN_NONE;
    cal_result_t r;

    /* Capture baseline with all relays guaranteed off. */
    set_axis_ns(AXIS_OFF);
    set_axis_ew(AXIS_OFF);
    delay_ms(200);
    current_idle = adc_read_avg(ADC_CH_STALL, 16);

    r = cal_measure_axis(CAL_AXIS_NS,
                         "Cal NS zero", "Cal NS ext", "Cal NS ret",
                         &ns_stroke_ms, &prev);
    if (r != CAL_OK) return r;
    cal_inter_step_wait(&prev);

    r = cal_measure_axis(CAL_AXIS_EW,
                         "Cal EW zero", "Cal EW ext", "Cal EW ret",
                         &ew_stroke_ms, &prev);
    if (r != CAL_OK) return r;

    /* Default horizontal = mid-stroke; user can override via Save Horizontal. */
    horiz_ns_pct = 50;
    horiz_ew_pct = 50;

    config_save();
    return CAL_OK;
}

/* Boot-time auto-zero: drive both axes to retract endstop.  Re-establishes
 * the position-tracking origin after every power cycle.  Called once
 * from main() at startup when calibration data is present. */
static cal_result_t boot_zero(void) {
    button_t prev = BTN_NONE;
    cal_result_t r;

    set_axis_ns(AXIS_OFF);
    set_axis_ew(AXIS_OFF);
    delay_ms(200);
    current_idle = adc_read_avg(ADC_CH_STALL, 16);

    r = cal_zero_axis(CAL_AXIS_NS, "Boot zero NS", &prev);
    if (r != CAL_OK) {
        set_axis_ns(AXIS_OFF);
        return r;
    }
    cal_inter_step_wait(&prev);

    r = cal_zero_axis(CAL_AXIS_EW, "Boot zero EW", &prev);
    set_axis_ew(AXIS_OFF);
    return r;
}

/* ---- Version info ---- */
#define FIRMWARE_VERSION "EcoWorthy v0.2c "
/* __DATE__ expands to "Mmm DD YYYY" (e.g. "May 12 2026"), 11 chars. */
static const char build_date[] = __DATE__;

/* ---- State machine ---- */
typedef enum {
    ST_NO_CAL,
    ST_IDLE,
    ST_MENU,
    ST_TRACK,
    ST_STORM,
    ST_JOG,
    ST_CAL,
    ST_BTEST,           /* backlash characterization */
    ST_SETTINGS,        /* settings sub-menu */
    ST_SETTINGS_EDIT,   /* editing a single setting value */
    ST_VERSION
} state_t;

/* ---- Settings sub-menu ----
 * Three configurable values; backed by EEPROM bytes 8..10.  Each
 * setting has a name, a range (min, max), and an optional unit string
 * shown in the edit view.  Editing supports short-press = 1 unit and
 * press-and-hold = auto-repeat after a 500 ms grace period.
 */
typedef enum {
    SET_WIND_STORM = 0,
    SET_WIND_RELEASE,
    SET_STORM_DWELL,
    SET_TRACK_THRESH,
    SET_ROLE,              /* new */
    SET_WIND_SOURCE,       /* new */
    SET_COUNT
} setting_t;

typedef struct {
    const char *short_label;  /* shown in the list (up to 12 chars) */
    const char *full_label;   /* shown when editing (up to 16 chars) */
    const char *unit;
    unsigned char min;
    unsigned char max;
} setting_def_t;

static const setting_def_t setting_defs[SET_COUNT] = {
    { "W.Storm",  "Wind storm",     "m/s", WIND_STORM_MIN,   WIND_STORM_MAX   },
    { "W.Rel",    "Wind release",   "m/s", WIND_RELEASE_MIN, WIND_RELEASE_MAX },
    { "S.Dwell",  "Storm dwell",    "min", DWELL_MIN_MIN,    DWELL_MIN_MAX    },
    { "TrkThr",   "Track thresh",   "adc", TRACK_THRESH_MIN, TRACK_THRESH_MAX },
    { "Role",     "Role 1=P 2=S",   "",    ROLE_MIN,         ROLE_MAX         },
    { "WSrc",     "Wind src 0L 1R", "",    WIND_SOURCE_MIN,  WIND_SOURCE_MAX  },
};

static unsigned char setting_get(setting_t s) {
    switch (s) {
        case SET_WIND_STORM:   return wind_storm_mps;
        case SET_WIND_RELEASE: return wind_release_mps;
        case SET_STORM_DWELL:  return storm_dwell_min;
        case SET_TRACK_THRESH: return track_thresh;
        case SET_ROLE:         return role;            /* new */
        case SET_WIND_SOURCE:  return wind_source;     /* new */
        default:               return 0;
    }
}

static void setting_set(setting_t s, unsigned char v) {
    switch (s) {
        case SET_WIND_STORM:   wind_storm_mps   = v; break;
        case SET_WIND_RELEASE: wind_release_mps = v; break;
        case SET_STORM_DWELL:  storm_dwell_min  = v; break;
        case SET_TRACK_THRESH: track_thresh     = v; break;
        case SET_ROLE:         role             = v; break;   /* new */
        case SET_WIND_SOURCE:  wind_source      = v; break;   /* new */
        default: break;
    }
}

/* Edit-mode state.  Loaded when ST_SETTINGS dispatches via SET. */
static __xdata unsigned char edit_setting_idx = 0;
static __xdata unsigned char edit_value = 0;
static __xdata unsigned char edit_original = 0;

/* Settings list scroll state (separate from main-menu window vars). */
static __xdata unsigned char set_win_top  = 0;
static __xdata unsigned char set_cursor_r = 0;

/* Press-and-hold tracking for auto-repeat in ST_SETTINGS_EDIT.  Reset
 * whenever the held button changes.  Holds for >= HOLD_DELAY_TICKS
 * generate repeat events every REPEAT_PERIOD_TICKS. */
static __xdata button_t      btn_hold_target = BTN_NONE;
static __xdata unsigned int  btn_hold_ticks  = 0;
#define HOLD_DELAY_TICKS     10   /* 10 * 50ms = 500ms grace before auto-repeat */
#define REPEAT_PERIOD_TICKS   2   /*  2 * 50ms = 100ms = 10 steps/sec */

/* Returns nonzero on edge press of `which`, OR when held past the
 * grace period at a REPEAT_PERIOD_TICKS-aligned tick.  Auto-repeat is
 * driven by the global btn_hold_target/btn_hold_ticks pair, so calling
 * code only needs to ask "should I step now?". */
static unsigned char btn_step_now(button_t which, button_t pressed) {
    if (pressed == which) return 1;
    if (btn_hold_target == which &&
        btn_hold_ticks  >= HOLD_DELAY_TICKS) {
        unsigned int over = btn_hold_ticks - HOLD_DELAY_TICKS;
        if ((over % REPEAT_PERIOD_TICKS) == 0) return 1;
    }
    return 0;
}

/* ST_IDLE screen.  Extracted so its sensor-read locals get a separate
 * overlay region (main()'s DSEG frame is at the budget limit). */
static void idle_screen(void) {
    static __xdata unsigned int sun_n, sun_s, sun_e, sun_w, wind;
    static __xdata unsigned char mps;

    sun_n = adc_read(ADC_CH_SUN_N);
    sun_s = adc_read(ADC_CH_SUN_S);
    sun_e = adc_read(ADC_CH_SUN_E);
    sun_w = adc_read(ADC_CH_SUN_W);
    wind  = adc_read(ADC_CH_WIND);

    lcd_goto(0, 0);
    lcd_putc('N'); lcd_putc('0' + (sun_n / 100));
    lcd_putc('0' + ((sun_n / 10) % 10)); lcd_putc('0' + (sun_n % 10));
    lcd_putc('S'); lcd_putc('0' + (sun_s / 100));
    lcd_putc('0' + ((sun_s / 10) % 10)); lcd_putc('0' + (sun_s % 10));
    lcd_putc('E'); lcd_putc('0' + (sun_e / 100));
    lcd_putc('0' + ((sun_e / 10) % 10)); lcd_putc('0' + (sun_e % 10));
    lcd_putc('W'); lcd_putc('0' + (sun_w / 100));
    lcd_putc('0' + ((sun_w / 10) % 10)); lcd_putc('0' + (sun_w % 10));

    mps = wind_mps(wind);
    lcd_goto(1, 0);
    lcd_print("Idle    Wnd=");
    lcd_putc('0' + (mps / 10));
    lcd_putc('0' + (mps % 10));
    lcd_print("  ");
}

/* ST_TRACK body.  Extracted so its locals get a separate overlay
 * region (main()'s DSEG frame is at the budget limit). */
static void track_tick(button_t pressed, state_t *state) {
    static __xdata unsigned long now;
    static __xdata unsigned int sn, ss, se, sw;

    now = millis();

    /* End expired pulses. */
    if (ns_pulse_end_ms != 0 && now >= ns_pulse_end_ms) {
        set_axis_ns(AXIS_OFF);
        ns_pulse_end_ms = 0;
    }
    if (ew_pulse_end_ms != 0 && now >= ew_pulse_end_ms) {
        set_axis_ew(AXIS_OFF);
        ew_pulse_end_ms = 0;
    }

    /* Periodic sensor read + correction decision. */
    if (now - track_last_check_ms >= TRACK_CHECK_PERIOD_MS) {
        track_last_check_ms = now;
        sn = adc_read_avg(ADC_CH_SUN_N, TRACK_SENSOR_AVG);
        ss = adc_read_avg(ADC_CH_SUN_S, TRACK_SENSOR_AVG);
        se = adc_read_avg(ADC_CH_SUN_E, TRACK_SENSOR_AVG);
        sw = adc_read_avg(ADC_CH_SUN_W, TRACK_SENSOR_AVG);
        track_dN = (int)sn - (int)ss;
        track_dE = (int)se - (int)sw;

        if (ns_state == AXIS_OFF && ns_pulse_end_ms == 0 && !ns_duty_locked()) {
            if (track_dN > (int)track_thresh) {
                set_axis_ns(AXIS_FWD);
                ns_pulse_end_ms = now + TRACK_PULSE_MS;
            } else if (track_dN < -(int)track_thresh) {
                set_axis_ns(AXIS_REV);
                ns_pulse_end_ms = now + TRACK_PULSE_MS;
            }
        }
        if (ew_state == AXIS_OFF && ew_pulse_end_ms == 0 && !ew_duty_locked()) {
            if (track_dE > (int)track_thresh) {
                set_axis_ew(AXIS_FWD);
                ew_pulse_end_ms = now + TRACK_PULSE_MS;
            } else if (track_dE < -(int)track_thresh) {
                set_axis_ew(AXIS_REV);
                ew_pulse_end_ms = now + TRACK_PULSE_MS;
            }
        }
    }

    /* Row 0: "Track  NS=N EW=E" -- 16 chars. */
    lcd_goto(0, 0);
    lcd_print("Track  NS=");
    lcd_putc(ns_char(ns_state));
    lcd_print(" EW=");
    lcd_putc(ew_char(ew_state));

    /* Row 1: "dN=+045 dE=-012 " -- 16 chars. */
    lcd_goto(1, 0);
    lcd_print("dN=");
    lcd_print_sint3(track_dN);
    lcd_print(" dE=");
    lcd_print_sint3(track_dE);
    lcd_putc(' ');

    if (pressed == BTN_QUIT) {
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);
        ns_pulse_end_ms = 0;
        ew_pulse_end_ms = 0;
        lcd_clear();
        *state = ST_MENU;
    }
}

/* Main-loop wind watchdog.  Runs every iteration regardless of mode.
 * On a storm-strength gust, force ST_STORM (PARKING) and reset duty
 * (safety > thermal).  No-op if already storming or uncalibrated
 * (can't compute a horizontal target without stroke times). */
/* Wind only changes over hundreds of ms; sampling it every ~50 ms
 * loop was pointless AND poisoned the very next button ADC read via
 * the WIND->BUTTON mux switch.  Sample at ~1 Hz: still catches a
 * building gust well before it can damage the array, and 98% of loop
 * iterations no longer switch the ADC away from the button channel. */
#define STORM_SCAN_MS  1000UL

static void storm_check(state_t *state) {
    static __xdata unsigned char w;
    static __xdata unsigned long storm_scan_last = 0;
    unsigned long now;

    now = millis();
    if (now - storm_scan_last < STORM_SCAN_MS) return;
    storm_scan_last = now;

    /* Forced storm: enter ST_STORM on operator !park OR auto wind-failsafe. */
    if ((storm_forced || wind_failsafe) && *state != ST_STORM) {
        ns_duty_on_ms = 0; ew_duty_on_ms = 0;
        ns_duty_lockout_end = 0; ew_duty_lockout_end = 0;
        duty_last_tick_ms = millis();
        ns_pulse_end_ms = 0; ew_pulse_end_ms = 0;
        storm_phase = STORM_PARKING;
        storm_parking = 1;
        lcd_clear();
        *state = ST_STORM;
        return;
    }

    /* Always sample + cache wind (UI uses the cache).  Storm action is
     * gated below; the cache update is unconditional so jog/idle still
     * see live wind even when uncalibrated or already storming. */
    if (wind_source == 0) {
        /* Local: read sensor as before */
        w = wind_mps(adc_read_avg(ADC_CH_WIND, 8));
        wind_mps_cached = w;
    } else {
        /* Remote: use cached value from !wind= broadcast.  Failsafe
         * arms after REMOTE_WIND_TIMEOUT_MS of silence and auto-clears
         * the moment a fresh broadcast arrives.  Operator !park is a
         * separate flag that stays sticky until !release. */
        unsigned long age = now - remote_wind_last_update_ms;
        if (remote_wind_last_update_ms == 0 || age > REMOTE_WIND_TIMEOUT_MS) {
            wind_failsafe = 1;
            w = wind_storm_mps;   /* report at-threshold so storm logic proceeds */
        } else {
            wind_failsafe = 0;
            w = remote_wind_mps;
            wind_mps_cached = w;  /* mirror to display */
        }
    }

    if (*state == ST_STORM) return;
    if (!cfg_valid) return;

    if (w >= wind_storm_mps) {
        ns_duty_on_ms = 0; ew_duty_on_ms = 0;
        ns_duty_lockout_end = 0; ew_duty_lockout_end = 0;
        duty_last_tick_ms = millis();
        ns_pulse_end_ms = 0; ew_pulse_end_ms = 0;  /* cancel track pulses */
        storm_phase = STORM_PARKING;
        storm_parking = 1;
        lcd_clear();
        *state = ST_STORM;
    }
}

/* ST_STORM body.  PARKING: seek both axes to the horizontal target.
 * HOLDING: axes off, run the resettable dwell timer.  Exits only to
 * ST_TRACK when wind has stayed below release for storm_dwell_min. */
static void storm_tick(state_t *state) {
    static __xdata unsigned long now, tgt_ns, tgt_ew, elapsed, need, remain;
    static __xdata unsigned char w, ns_ok, ew_ok;

    now = millis();
    w   = wind_mps(adc_read_avg(ADC_CH_WIND, 8));

    if (storm_phase == STORM_PARKING) {
        tgt_ns = (unsigned long)horiz_ns_pct * ns_stroke_ms / 100UL;
        tgt_ew = (unsigned long)horiz_ew_pct * ew_stroke_ms / 100UL;

        ns_ok = 0;
        if ((unsigned long)ns_pos_ms + STORM_POS_TOL_MS < tgt_ns) {
            set_axis_ns(AXIS_FWD);
        } else if ((unsigned long)ns_pos_ms > tgt_ns + STORM_POS_TOL_MS) {
            set_axis_ns(AXIS_REV);
        } else {
            set_axis_ns(AXIS_OFF);
            ns_ok = 1;
        }

        ew_ok = 0;
        if ((unsigned long)ew_pos_ms + STORM_POS_TOL_MS < tgt_ew) {
            set_axis_ew(AXIS_FWD);
        } else if ((unsigned long)ew_pos_ms > tgt_ew + STORM_POS_TOL_MS) {
            set_axis_ew(AXIS_REV);
        } else {
            set_axis_ew(AXIS_OFF);
            ew_ok = 1;
        }

        if (ns_ok && ew_ok) {
            storm_phase = STORM_HOLDING;
            storm_parking = 0;            /* resume duty accounting */
            storm_dwell_start_ms = now;   /* start dwell clock */
        }

        lcd_goto(0, 0); lcd_print_padded("STORM  PARK", 16);
        lcd_goto(1, 0);
        lcd_print("Wnd=");
        lcd_putc('0' + (w / 10));
        lcd_putc('0' + (w % 10));
        lcd_print(" ->horiz  ");
    } else {
        /* HOLDING: axes off, monitor wind + dwell. */
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);

        if (w >= wind_release_mps) {
            storm_dwell_start_ms = now;   /* any spike resets the dwell */
        }
        elapsed = now - storm_dwell_start_ms;
        need    = (unsigned long)storm_dwell_min * 60000UL;

        if (elapsed >= need && !storm_forced && !wind_failsafe) {
            lcd_clear();
            track_last_check_ms = millis();   /* fresh tracking eval */
            *state = ST_TRACK;
            return;
        }

        /* Defensive: clamp before the unsigned subtraction so a stuck
         * storm (e.g. lingering storm_forced from !park) does not
         * underflow the display to "99m". */
        remain = (elapsed >= need) ? 0
                                   : (need - elapsed + 59999UL) / 60000UL;
        if (remain > 99) remain = 99;
        lcd_goto(0, 0); lcd_print_padded("STORM  HOLD", 16);
        lcd_goto(1, 0);
        lcd_print("Wnd=");
        lcd_putc('0' + (w / 10));
        lcd_putc('0' + (w % 10));
        lcd_print(" Dwl=");
        lcd_putc('0' + ((unsigned char)remain / 10));
        lcd_putc('0' + ((unsigned char)remain % 10));
        lcd_print("m ");
    }
}

/* ---- HA status responder (Phase 3-4, read-only) ----
 * On a valid framed `?` poll, reply with one framed status packet:
 *   az=<ew%> el=<ns%> wind=<mps> mode=<m>
 * az = E/W axis position % (rotation), el = N/S axis position % (tilt).
 * Fully ISR-decoupled: the parser runs in the main loop, the reply is
 * enqueued to the TX ring; nothing blocks the 50 ms state machine.
 * During blocking cal/boot the loop doesn't run, so HA simply sees the
 * sensor go stale (ESPHome marks unavailable) until cal finishes. */
static const char *uart_mode_str(state_t st) {
    switch (st) {
        case ST_NO_CAL:        return "nocal";
        case ST_IDLE:          return "idle";
        case ST_MENU:          return "menu";
        case ST_TRACK:         return "track";
        case ST_STORM:         return "storm";
        case ST_CAL:           return "cal";
        case ST_BTEST:         return "btest";
        case ST_SETTINGS:      return "set";
        case ST_SETTINGS_EDIT: return "set";
        case ST_JOG:           return "jog";
        case ST_VERSION:       return "ver";
        default:               return "?";
    }
}

static char *uart_app_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

static char *uart_app_u8(char *p, unsigned char v) {
    if (v >= 100) { *p++ = '0' + v / 100; v %= 100;
                    *p++ = '0' + v / 10;  *p++ = '0' + v % 10; }
    else if (v >= 10) { *p++ = '0' + v / 10; *p++ = '0' + v % 10; }
    else { *p++ = '0' + v; }
    return p;
}

static char *uart_app_u16(char *p, unsigned int v) {
    char tmp[6];
    int n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

static void uart_cfg_send_value(unsigned char field_id,
                                unsigned int val_u16) {
    static __xdata char buf[40];
    char *p = buf;
    p = uart_app_str(p, "cfg id=");
    p = uart_app_u8(p, field_id);
    p = uart_app_str(p, " val=");
    p = uart_app_u16(p, val_u16);
    *p = '\0';
    uart_send_frame(buf);
}

static void uart_cfg_send_ack(unsigned char field_id,
                              unsigned char status) {
    static __xdata char buf[40];
    char *p = buf;
    p = uart_app_str(p, "cfg ack id=");
    p = uart_app_u8(p, field_id);
    p = uart_app_str(p, " st=");
    p = uart_app_u8(p, status);
    *p = '\0';
    uart_send_frame(buf);
}

static void uart_status_send(state_t st) {
    static __xdata char buf[40];
    char *p = buf;
    p = uart_app_str(p, "az=");
    p = uart_app_u8(p, pos_to_pct(ew_pos_ms, ew_stroke_ms));
    p = uart_app_str(p, " el=");
    p = uart_app_u8(p, pos_to_pct(ns_pos_ms, ns_stroke_ms));
    p = uart_app_str(p, " wind=");
    p = uart_app_u8(p, wind_mps(adc_read_avg(ADC_CH_WIND, 8)));
    p = uart_app_str(p, " mode=");
    p = uart_app_str(p, uart_mode_str(st));
    *p = '\0';
    uart_send_frame(buf);
}

/* Parse decimal digits from p; return value and advance *p past them.
 * Returns -1 on no-digits.  Caps at 9999 to bound. */
static int parse_u_advance(const char **p) {
    int v = 0;
    int saw = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        if (v > 9999) v = 9999;
        (*p)++;
        saw = 1;
    }
    return saw ? v : -1;
}

/* Dispatch a validated framed payload.  ? = status request; ! =
 * command (sub-parsed by token).  Other payloads are dropped. */
static void uart_cmd_dispatch(state_t *state) {
    const char *p = uart_frame_buf;

    /* ? = status poll (existing behavior) */
    if (p[0] == '?' && p[1] == '\0') {
        uart_status_send(*state);
        return;
    }

    /* !park / !release: set/clear the forced-storm flag */
    if (p[0] != '!') return;
    if (strncmp(p + 1, "park", 4) == 0 && p[5] == '\0') {
        storm_forced = 1;
        return;
    }
    if (strncmp(p + 1, "release", 7) == 0 && p[8] == '\0') {
        storm_forced = 0;
        return;
    }

    /* !wind=NN -- remote wind update from gateway */
    if (strncmp(p + 1, "wind=", 5) == 0) {
        const char *q = p + 6;
        int v = parse_u_advance(&q);
        if (v >= 0) {
            remote_wind_mps = (v > 99) ? 99 : (unsigned char)v;
            remote_wind_last_update_ms = millis();
        }
        return;
    }

    /* !stop -- emergency stop both axes */
    if (strncmp(p + 1, "stop", 4) == 0 && p[5] == '\0') {
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);
        ns_pulse_end_ms = 0;
        ew_pulse_end_ms = 0;
        goto_active = 0;
        return;
    }

    /* !goto az=NN el=NN -- goto position percentages.
     * Respects storm/duty interlocks; clamps to [0,100]. */
    if (strncmp(p + 1, "goto ", 5) == 0) {
        int az_v = -1, el_v = -1;
        const char *q = p + 6;
        while (*q) {
            if (q[0] == 'a' && q[1] == 'z' && q[2] == '=') { q += 3; az_v = parse_u_advance(&q); }
            else if (q[0] == 'e' && q[1] == 'l' && q[2] == '=') { q += 3; el_v = parse_u_advance(&q); }
            else if (*q == ' ') q++;
            else q++;
        }
        if (az_v < 0 || el_v < 0) return;
        if (az_v > 100) az_v = 100;
        if (el_v > 100) el_v = 100;
        if (*state == ST_STORM || ns_duty_locked() || ew_duty_locked()) return;
        goto_az_target_pct = (unsigned char)az_v;
        goto_el_target_pct = (unsigned char)el_v;
        goto_active = 1;
        return;
    }

    /* !jog ax=N dir=+/- ms=NNN -- timed pulse on one axis */
    if (strncmp(p + 1, "jog ", 4) == 0) {
        const char *q = p + 5;
        int ax = -1, dir_pos = -1, dur_ms = -1;
        while (*q) {
            if (q[0] == 'a' && q[1] == 'x' && q[2] == '=') { q += 3; ax = parse_u_advance(&q); }
            else if (q[0] == 'd' && q[1] == 'i' && q[2] == 'r' && q[3] == '=') {
                q += 4;
                if (*q == '+') { dir_pos = 1; q++; }
                else if (*q == '-') { dir_pos = 0; q++; }
            }
            else if (q[0] == 'm' && q[1] == 's' && q[2] == '=') { q += 3; dur_ms = parse_u_advance(&q); }
            else if (*q == ' ') q++;
            else q++;
        }
        if (ax < 0 || dir_pos < 0 || dur_ms < 0) return;
        if (dur_ms > 2500) dur_ms = 2500;
        if (*state == ST_STORM) return;
        if (ax == 0) {
            if (ns_duty_locked()) return;
            set_axis_ns(dir_pos ? AXIS_FWD : AXIS_REV);
            ns_pulse_end_ms = millis() + (unsigned long)dur_ms;
        } else if (ax == 1) {
            if (ew_duty_locked()) return;
            set_axis_ew(dir_pos ? AXIS_FWD : AXIS_REV);
            ew_pulse_end_ms = millis() + (unsigned long)dur_ms;
        }
        return;
    }

    /* !cal -- trigger calibration (only from IDLE, not in storm) */
    if (strncmp(p + 1, "cal", 3) == 0 && p[4] == '\0') {
        if (*state == ST_IDLE && !storm_forced) {
            goto_active = 0;     /* cancel any pending goto */
            *state = ST_CAL;
        }
        return;
    }

    /* !cfg get id=NN -- read config field */
    if (strncmp(p + 1, "cfg get id=", 11) == 0) {
        const char *q = p + 12;
        int id = parse_u_advance(&q);
        if (id < 0) return;
        switch (id) {
            case CFG_F_WIND_STORM:    uart_cfg_send_value(id, wind_storm_mps); return;
            case CFG_F_WIND_RELEASE:  uart_cfg_send_value(id, wind_release_mps); return;
            case CFG_F_STORM_DWELL:   uart_cfg_send_value(id, storm_dwell_min); return;
            case CFG_F_TRACK_THRESH:  uart_cfg_send_value(id, track_thresh); return;
            case CFG_F_NS_STROKE:     uart_cfg_send_value(id, ns_stroke_ms); return;
            case CFG_F_EW_STROKE:     uart_cfg_send_value(id, ew_stroke_ms); return;
            case CFG_F_HORIZ_NS:      uart_cfg_send_value(id, horiz_ns_pct); return;
            case CFG_F_HORIZ_EW:      uart_cfg_send_value(id, horiz_ew_pct); return;
            case CFG_F_ROLE:          uart_cfg_send_value(id, role); return;
            case CFG_F_WIND_SOURCE:   uart_cfg_send_value(id, wind_source); return;
            default:                  uart_cfg_send_ack((unsigned char)id, CFG_STATUS_UNKNOWN); return;
        }
    }

    /* !cfg set id=NN val=NN */
    if (strncmp(p + 1, "cfg set id=", 11) == 0) {
        const char *q = p + 12;
        int id = parse_u_advance(&q);
        if (id < 0) return;
        /* Skip " val=" */
        while (*q == ' ') q++;
        if (q[0] != 'v' || q[1] != 'a' || q[2] != 'l' || q[3] != '=') return;
        q += 4;
        int val = parse_u_advance(&q);
        if (val < 0) return;
        unsigned char status = CFG_STATUS_OK;
        switch (id) {
            case CFG_F_WIND_STORM:
                if (val < WIND_STORM_MIN || val > WIND_STORM_MAX) status = CFG_STATUS_RANGE;
                else { wind_storm_mps = (unsigned char)val; config_save(); }
                break;
            case CFG_F_WIND_RELEASE:
                if (val > WIND_RELEASE_MAX) status = CFG_STATUS_RANGE;
                else { wind_release_mps = (unsigned char)val; config_save(); }
                break;
            case CFG_F_STORM_DWELL:
                if (val < DWELL_MIN_MIN || val > DWELL_MIN_MAX) status = CFG_STATUS_RANGE;
                else { storm_dwell_min = (unsigned char)val; config_save(); }
                break;
            case CFG_F_TRACK_THRESH:
                if (val < TRACK_THRESH_MIN || val > TRACK_THRESH_MAX) status = CFG_STATUS_RANGE;
                else { track_thresh = (unsigned char)val; config_save(); }
                break;
            /* All other fields read-only over mesh */
            case CFG_F_NS_STROKE:
            case CFG_F_EW_STROKE:
            case CFG_F_HORIZ_NS:
            case CFG_F_HORIZ_EW:
            case CFG_F_ROLE:
            case CFG_F_WIND_SOURCE:
                status = CFG_STATUS_RO;
                break;
            default:
                status = CFG_STATUS_UNKNOWN;
        }
        uart_cfg_send_ack((unsigned char)id, status);
        return;
    }
}

/* Drain RX through the frame parser; on a valid framed payload,
 * dispatch via uart_cmd_dispatch.  Call once per main-loop iteration. */
static void uart_service(state_t *state) {
    uart_poll_frames();
    if (uart_frame_ready) {
        uart_cmd_dispatch(state);
        uart_frame_ready = 0;
    }
}

/* ST_SETTINGS list body.  Extracted to keep main()'s overlay frame
 * within the DSEG budget.  Mutates *state on SET/QUIT. */
static void settings_list_tick(button_t pressed, state_t *state) {
    static __xdata unsigned char row, idx, v, sel;

    for (row = 0; row < 2; row++) {
        idx = set_win_top + row;
        lcd_goto(row, 0);
        lcd_putc(set_cursor_r == row ? '>' : ' ');
        lcd_putc(' ');
        if (idx < SET_COUNT) {
            v = setting_get((setting_t)idx);
            lcd_print_padded(setting_defs[idx].short_label, 12);
            lcd_putc('0' + (v / 10));
            lcd_putc('0' + (v % 10));
        } else {
            lcd_print_padded("", 14);
        }
    }

    if (pressed == BTN_NORTH) {
        if (set_cursor_r == 1) {
            set_cursor_r = 0;
        } else if (set_win_top > 0) {
            set_win_top--;
        }
    } else if (pressed == BTN_SOUTH) {
        if (set_cursor_r == 0 && set_win_top + 1 < SET_COUNT) {
            set_cursor_r = 1;
        } else if (set_win_top + 2 < SET_COUNT) {
            set_win_top++;
        }
    } else if (pressed == BTN_SET) {
        sel = set_win_top + set_cursor_r;
        if (sel < SET_COUNT) {
            edit_setting_idx = sel;
            edit_original    = setting_get((setting_t)sel);
            edit_value       = edit_original;
            lcd_clear();
            *state = ST_SETTINGS_EDIT;
        }
    } else if (pressed == BTN_QUIT) {
        lcd_clear();
        *state = ST_MENU;
    }
}

/* ST_SETTINGS_EDIT body, extracted to keep main()'s overlay frame
 * within the DSEG budget.  Mutates *state to ST_SETTINGS on SET (save)
 * or QUIT (revert). */
static void settings_edit_tick(button_t pressed, state_t *state) {
    static __xdata setting_t s;
    static __xdata unsigned char mn, mx;

    s  = (setting_t)edit_setting_idx;
    mn = setting_defs[s].min;
    mx = setting_defs[s].max;

    if (btn_step_now(BTN_NORTH, pressed) && edit_value < mx) edit_value++;
    if (btn_step_now(BTN_SOUTH, pressed) && edit_value > mn) edit_value--;

    lcd_goto(0, 0);
    lcd_print_padded(setting_defs[s].full_label, 16);
    lcd_goto(1, 0);
    lcd_print("   ");
    lcd_putc('0' + (edit_value / 10));
    lcd_putc('0' + (edit_value % 10));
    lcd_putc(' ');
    lcd_print_padded(setting_defs[s].unit, 10);

    if (pressed == BTN_SET) {
        setting_set(s, edit_value);
        config_save();
        lcd_clear();
        *state = ST_SETTINGS;
    } else if (pressed == BTN_QUIT) {
        setting_set(s, edit_original);  /* revert */
        lcd_clear();
        *state = ST_SETTINGS;
    }
}

/* ST_JOG body.  Extracted to keep main()'s overlay frame within the
 * DSEG budget.  `idle_timeout` is precomputed by the caller (the
 * idle_ticks var is a main-scope alias not visible here). */
static void jog_tick(button_t pressed, button_t curr_button,
                     unsigned char idle_timeout, state_t *state) {
    static __xdata axis_state_t tgt_ns, tgt_ew;
    static __xdata unsigned char ns_pct, ew_pct;

    tgt_ns = AXIS_OFF;
    tgt_ew = AXIS_OFF;
    switch (curr_button) {
        case BTN_NORTH: tgt_ns = AXIS_FWD; break;
        case BTN_SOUTH: tgt_ns = AXIS_REV; break;
        case BTN_EAST:  tgt_ew = AXIS_FWD; break;
        case BTN_WEST:  tgt_ew = AXIS_REV; break;
        default: break;
    }
    /* Respect duty lockout even in manual jog. */
    if (ns_duty_locked()) tgt_ns = AXIS_OFF;
    if (ew_duty_locked()) tgt_ew = AXIS_OFF;
    set_axis_ns(tgt_ns);
    set_axis_ew(tgt_ew);

    /* Save Horizontal: SET while both axes off -> capture pos -> EEPROM. */
    if (pressed == BTN_SET &&
        ns_state == AXIS_OFF && ew_state == AXIS_OFF) {
        horiz_ns_pct = pos_to_pct(ns_pos_ms, ns_stroke_ms);
        horiz_ew_pct = pos_to_pct(ew_pos_ms, ew_stroke_ms);
        config_save();
        lcd_clear();
        lcd_goto(0, 0); lcd_print_padded("Saved horiz", 16);
        lcd_goto(1, 0);
        lcd_print("NS=");
        lcd_putc('0' + (horiz_ns_pct / 100));
        lcd_putc('0' + ((horiz_ns_pct / 10) % 10));
        lcd_putc('0' + (horiz_ns_pct % 10));
        lcd_print(" EW=");
        lcd_putc('0' + (horiz_ew_pct / 100));
        lcd_putc('0' + ((horiz_ew_pct / 10) % 10));
        lcd_putc('0' + (horiz_ew_pct % 10));
        lcd_print("  ");
        delay_ms(1500);
        lcd_clear();
        pos_last_tick_ms = millis();
    }

    /* Layout A: position % on row 0, axis arrows on row 1.
     * NOTE: no ADC reads here on purpose -- a per-loop WIND read would
     * mux-switch the ADC right before the next button sample, jittering
     * the high-Z button ladder and flickering curr_button (jog needs it
     * latched every iteration, unlike edge-driven menus). */
    ns_pct = pos_to_pct(ns_pos_ms, ns_stroke_ms);
    ew_pct = pos_to_pct(ew_pos_ms, ew_stroke_ms);

    lcd_goto(0, 0);
    lcd_print("NS=");
    lcd_putc('0' + (ns_pct / 100));
    lcd_putc('0' + ((ns_pct / 10) % 10));
    lcd_putc('0' + (ns_pct % 10));
    lcd_print(" EW=");
    lcd_putc('0' + (ew_pct / 100));
    lcd_putc('0' + ((ew_pct / 10) % 10));
    lcd_putc('0' + (ew_pct % 10));
    lcd_print("  ");

    /* Row 1: "NS=N EW=E W=NN  " -- 15 chars; col 15 reserved for the
     * padlock overlay drawn in the main loop after this render.  Wind
     * comes from the 1 Hz cache (no ADC read here -- a per-loop ADC
     * channel switch would jitter the next button read and kill jog). */
    lcd_goto(1, 0);
    lcd_print("NS=");
    lcd_putc(ns_char(ns_state));
    lcd_print(" EW=");
    lcd_putc(ew_char(ew_state));
    lcd_print(" W=");
    lcd_putc('0' + (wind_mps_cached / 10));
    lcd_putc('0' + (wind_mps_cached % 10));
    lcd_putc(' ');

    if (pressed == BTN_QUIT) {
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);
        lcd_clear();
        *state = ST_MENU;
    } else if (idle_timeout) {
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);
        lcd_clear();
        *state = ST_IDLE;
    }
}

typedef enum {
    MENU_TRACK = 0,
    MENU_JOG,
    MENU_CALIBRATE,
    MENU_BLASH,
    MENU_SETTINGS,
    MENU_VERSION,
    MENU_COUNT
} menu_item_t;

static const char * const menu_labels[MENU_COUNT] = {
    "Track",
    "Jog",
    "Calibrate",
    "Backlash",
    "Settings",
    "Version"
};

/* Backlash test state.  Persistent across menu entries (resets on menu
 * SET); btest_pulse_ms increments by 20 each SET-run inside ST_BTEST. */
static __xdata cal_axis_t btest_axis = CAL_AXIS_NS;
static __xdata unsigned int btest_pulse_ms = 20;

#define INACTIVITY_LIMIT  600

/* Button debounce: require N consecutive identical classifications
 * before accepting a button value.  Filters out the misclassifications
 * that occur during release-ramp, when the analog bus voltage decays
 * monotonically through every other button's threshold on its way
 * back to 0.  At the 50 ms main-loop cadence, N=2 gives 100 ms
 * confirmation latency -- bump to 3 if misclassifications persist. */
#define BTN_DEBOUNCE_N  2

/* main()'s persistent loop state.  Hoisted to file-scope __xdata so
 * they don't compete for the tiny DSEG overlay region (only ~22 bytes
 * available after stack and reg-banks).  No semantic change -- main
 * never returns, so file scope vs. local scope is equivalent here. */
static __xdata state_t state_m;
static __xdata unsigned char window_top_m  = 0;
static __xdata unsigned char cursor_row_m  = 0;
static __xdata button_t      prev_button_m = BTN_NONE;
static __xdata button_t      db_candidate_m = BTN_NONE;
static __xdata unsigned char db_count_m    = 0;
static __xdata unsigned int  idle_ticks_m  = 0;

#define state        state_m
#define window_top   window_top_m
#define cursor_row   cursor_row_m
#define prev_button  prev_button_m
#define db_candidate db_candidate_m
#define db_count     db_count_m
#define idle_ticks   idle_ticks_m

void main(void) {

    /* Relay-safe boot. */
    P3 = 0x00;
    P5 &= ~(1 << 4);
    BUZZER = 1;

    /* Push-pull outputs.  Note P3 bit 4 (LCD_BL, pin 19) added. */
    P3M0 |=  (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7);
    P3M1 &= ~((1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7));
    P5M0 |=  (1 << 4);
    P5M1 &= ~(1 << 4);
    P1M0 |=  (1 << 7);
    P1M1 &= ~(1 << 7);
    P2M0 = 0xFF;
    P2M1 = 0x00;

    P1M1 |=  ANALOG_PINS;
    P1M0 &= ~ANALOG_PINS;
    P1ASF = ANALOG_PINS;

    /* P3.2 (pin 17, BRIDGE_IO) open-drain for the half-duplex bus.
     * Bit-level ops only -- P3 is split-purpose (relays/LCD share it). */
    P3M0 |= (1 << 2);
    P3M1 |= (1 << 2);
    BRIDGE_IO = 1;        /* release line (idle HIGH via breakout pull-up) */

    /* Backlight on for the duration of normal operation.  Future work:
     * dim or auto-off after inactivity to save power. */
    LCD_BL = 1;

    lcd_init();
    adc_init();
    timer0_init();        /* start 1 kHz ms counter for millis() */
#if P3_UART_ENABLE
    timer1_init();        /* start soft-UART bit clock (9600 8N1) */
    IT0 = 1;              /* INT0 = falling-edge (UART start bit) */
    EX0 = 1;              /* arm RX start-bit detection */
#endif
    config_load();        /* pull stroke times + horizontal from EEPROM if cal'd */

    /* Defense: explicitly zero Phase-4 state that lives in XISEG. If SDCC's
     * XDATA init runtime misses these (or if a quick power-cycle leaves
     * stale SRAM values), starting with non-zero storm_forced/goto/remote
     * bits would falsely re-enter storm or fire stale gotos at boot. */
    storm_forced = 0;
    wind_failsafe = 0;
    remote_wind_mps = 0;
    remote_wind_last_update_ms = 0;
    goto_active = 0;

    /* Capture stall-current baseline with all relays guaranteed OFF.
     * Used by jog dI display and (re-captured) by calibration. */
    delay_ms(200);
    current_idle = adc_read_avg(ADC_CH_STALL, 16);

    config_valid_refresh();   /* prime the cached validity flag */
    state = cfg_valid ? ST_IDLE : ST_NO_CAL;

    /* If calibrated, run auto-zero before entering normal operation.
     * Failure (timeout or user QUIT) falls through to IDLE -- the user
     * can re-zero manually via the Calibrate menu item. */
    if (state == ST_IDLE) {
        cal_result_t r = boot_zero();
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);
        if (r != CAL_OK) {
            lcd_clear();
            lcd_goto(0, 0); lcd_print_padded("Zero failed", 16);
            lcd_goto(1, 0);
            lcd_print_padded((r == CAL_ABORTED) ? "(aborted)" : "(timeout)", 16);
            delay_ms(2000);
        }
    }
    position_reset();    /* zero pos vars + arm tick clocks */
    duty_cold_reset();   /* boot: motor cold -> interlock clear */
    lcd_clear();

    for (;;) {
        unsigned int btn_adc;
        button_t raw_button;
        button_t curr_button;
        button_t pressed;

#if P3_BTN_DEBUG
        {
            static __xdata unsigned int dbg_n = 0;
            unsigned int a  = adc_read_settled(ADC_CH_BUTTONS, 4);
            unsigned int a1 = adc_read(ADC_CH_BUTTONS);  /* raw, post-settle */
            button_t bc = button_classify(a);
            const char *nm =
                (bc == BTN_NONE)  ? "NONE " : (bc == BTN_SET)   ? "SET  " :
                (bc == BTN_QUIT)  ? "QUIT " : (bc == BTN_WEST)  ? "WEST " :
                (bc == BTN_EAST)  ? "EAST " : (bc == BTN_NORTH) ? "NORTH" :
                                    "SOUTH";
            dbg_n++;
            lcd_goto(0, 0);
            lcd_print("S=");
            lcd_putc('0' + (a / 1000));      lcd_putc('0' + ((a / 100) % 10));
            lcd_putc('0' + ((a / 10) % 10)); lcd_putc('0' + (a % 10));
            lcd_print(" R=");
            lcd_putc('0' + (a1 / 1000));      lcd_putc('0' + ((a1 / 100) % 10));
            lcd_putc('0' + ((a1 / 10) % 10)); lcd_putc('0' + (a1 % 10));
            lcd_print("  ");
            lcd_goto(1, 0);
            lcd_print(nm);
            lcd_print(" #");
            lcd_putc('0' + ((dbg_n / 100) % 10));
            lcd_putc('0' + ((dbg_n / 10) % 10));
            lcd_putc('0' + (dbg_n % 10));
            lcd_print("      ");
            delay_ms(80);
            continue;
        }
#endif

        /* Integrate position based on whatever axis state was in effect
         * for the time elapsed since the last loop iteration.  Must run
         * BEFORE any code that mutates ns_state / ew_state so the dt is
         * credited to the right state. */
        position_tick();

        /* HA goto: drive both axes toward goto_*_target_pct in percent
         * of stroke.  Stops each axis when within ~1% of target.
         * Clears goto_active when both axes reach. */
        if (goto_active) {
            /* az drives E/W rotation; el drives N/S tilt (CLAUDE.md quirk 5). */
            unsigned long tgt_ns_ms = (unsigned long)goto_el_target_pct * ns_stroke_ms / 100UL;
            unsigned long tgt_ew_ms = (unsigned long)goto_az_target_pct * ew_stroke_ms / 100UL;
            unsigned char ns_done = 0, ew_done = 0;
            if (ns_state == AXIS_OFF) {
                if ((unsigned long)ns_pos_ms + 200 < tgt_ns_ms) set_axis_ns(AXIS_FWD);
                else if ((unsigned long)ns_pos_ms > tgt_ns_ms + 200) set_axis_ns(AXIS_REV);
                else ns_done = 1;
            } else {
                if (ns_state == AXIS_FWD && (unsigned long)ns_pos_ms >= tgt_ns_ms) { set_axis_ns(AXIS_OFF); ns_done = 1; }
                if (ns_state == AXIS_REV && (unsigned long)ns_pos_ms <= tgt_ns_ms) { set_axis_ns(AXIS_OFF); ns_done = 1; }
            }
            if (ew_state == AXIS_OFF) {
                if ((unsigned long)ew_pos_ms + 200 < tgt_ew_ms) set_axis_ew(AXIS_FWD);
                else if ((unsigned long)ew_pos_ms > tgt_ew_ms + 200) set_axis_ew(AXIS_REV);
                else ew_done = 1;
            } else {
                if (ew_state == AXIS_FWD && (unsigned long)ew_pos_ms >= tgt_ew_ms) { set_axis_ew(AXIS_OFF); ew_done = 1; }
                if (ew_state == AXIS_REV && (unsigned long)ew_pos_ms <= tgt_ew_ms) { set_axis_ew(AXIS_OFF); ew_done = 1; }
            }
            if (ns_done && ew_done) goto_active = 0;
            /* Storm or duty lockout cancels the goto */
            if (state == ST_STORM || ns_duty_locked() || ew_duty_locked()) {
                goto_active = 0;
                set_axis_ns(AXIS_OFF);
                set_axis_ew(AXIS_OFF);
            }
        }

        duty_tick();   /* enforce motor on-time limit regardless of mode */
        storm_check(&state);  /* wind watchdog -- may force ST_STORM */

#if P3_UART_TX_TEST
        {
            static __xdata unsigned long uart_test_last = 0;
            unsigned long uart_test_now = millis();
            if (uart_test_now - uart_test_last >= 2000UL) {
                uart_test_last = uart_test_now;
                uart_tx_str("ECOWORTHY TX TEST\r\n");
            }
        }
#endif
#if P3_UART_RX_ECHO
        while (uart_rx_avail()) {
            uart_tx_byte(uart_rx_get());
        }
#endif
#if P3_UART_ENABLE
        /* HA bridge: parse polls, reply with status.  Always runs;
         * fully non-blocking (parser + enqueue only). */
        uart_service(&state);
#endif
#if P3_UART_ENABLE && P3_UART_STATUS_TEST
        {
            static __xdata unsigned long uart_st_last = 0;
            unsigned long uart_st_now = millis();
            if (uart_st_now - uart_st_last >= 2000UL) {
                uart_st_last = uart_st_now;
                uart_status_send(state);
            }
        }
#endif

        btn_adc = adc_read_settled(ADC_CH_BUTTONS, 4);
        raw_button = button_classify(btn_adc);

        /* Debounce: confirm a new button only after BTN_DEBOUNCE_N
         * consecutive identical raw classifications.  Until then,
         * curr_button keeps its last confirmed value (which means a
         * release-ramp transient won't briefly register as a different
         * key). */
        if (raw_button == db_candidate) {
            if (db_count < 255) db_count++;
        } else {
            db_candidate = raw_button;
            db_count = 1;
        }
        curr_button = (db_count >= BTN_DEBOUNCE_N) ? db_candidate : prev_button;

        pressed = (curr_button != BTN_NONE && prev_button == BTN_NONE)
                  ? curr_button : BTN_NONE;
        prev_button = curr_button;

        /* Hold tracker: counts main-loop iterations while the same
         * (non-NONE) button stays debounced.  Read by ST_SETTINGS_EDIT
         * to gate auto-repeat. */
        if (curr_button != btn_hold_target) {
            btn_hold_target = curr_button;
            btn_hold_ticks = 0;
        } else if (curr_button != BTN_NONE && btn_hold_ticks < 65000) {
            btn_hold_ticks++;
        }

        if (curr_button != BTN_NONE) {
            idle_ticks = 0;
        } else if (idle_ticks < INACTIVITY_LIMIT) {
            idle_ticks++;
        }

        /* Backlight follows activity: any button press lights it; sustained
         * inactivity turns it off.  Same threshold as the menu auto-exit
         * so they happen together. */
        LCD_BL = (idle_ticks < INACTIVITY_LIMIT) ? 1 : 0;

        switch (state) {

        case ST_NO_CAL:
            lcd_goto(0, 0); lcd_print_padded("Not calibrated", 16);
            lcd_goto(1, 0); lcd_print_padded("Press SET to cal", 16);
            if (pressed == BTN_SET) {
                lcd_clear();
                state = ST_CAL;
            }
            break;

        case ST_IDLE:
            idle_screen();
            if (pressed == BTN_SET) {
                lcd_clear();
                window_top = 0;
                cursor_row = 0;
                idle_ticks = 0;
                state = ST_MENU;
            }
            break;

        case ST_MENU: {
            lcd_goto(0, 0);
            lcd_putc(cursor_row == 0 ? '>' : ' ');
            lcd_putc(' ');
            lcd_print_padded(menu_labels[window_top], 14);

            lcd_goto(1, 0);
            lcd_putc(cursor_row == 1 ? '>' : ' ');
            lcd_putc(' ');
            if (window_top + 1 < MENU_COUNT) {
                lcd_print_padded(menu_labels[window_top + 1], 14);
            } else {
                lcd_print_padded("", 14);
            }

            if (pressed == BTN_NORTH) {
                if (cursor_row == 1) {
                    cursor_row = 0;
                } else if (window_top > 0) {
                    window_top--;
                }
            } else if (pressed == BTN_SOUTH) {
                if (cursor_row == 0 && window_top + 1 < MENU_COUNT) {
                    cursor_row = 1;
                } else if (window_top + 2 < MENU_COUNT) {
                    window_top++;
                }
            } else if (pressed == BTN_SET) {
                unsigned char selected = window_top + cursor_row;
                lcd_clear();
                switch (selected) {
                    case MENU_TRACK:     state = ST_TRACK;         break;
                    case MENU_JOG:       state = ST_JOG;           break;
                    case MENU_CALIBRATE: state = ST_CAL;           break;
                    case MENU_BLASH:
                        state = ST_BTEST;
                        btest_axis = CAL_AXIS_NS;
                        btest_pulse_ms = 20;
                        break;
                    case MENU_SETTINGS:
                        state = ST_SETTINGS;
                        set_win_top = 0;
                        set_cursor_r = 0;
                        break;
                    case MENU_VERSION:   state = ST_VERSION;       break;
                    default: break;
                }
            } else if (pressed == BTN_QUIT) {
                lcd_clear();
                state = ST_IDLE;
            } else if (idle_ticks >= INACTIVITY_LIMIT) {
                lcd_clear();
                state = ST_IDLE;
            }
            break;
        }

        case ST_TRACK:
            track_tick(pressed, &state);
            break;

        case ST_STORM:
            storm_tick(&state);
            break;

        case ST_CAL: {
            /* Blocking sub-routine; takes 2-5 min to complete.  All relay
             * safety is handled inside calibrate().  On return:
             *   CAL_OK       -> config saved, advance to IDLE
             *   CAL_ABORTED  -> user QUIT; relays already off; revert path
             *                   depends on whether we had a prior config.
             *   CAL_TIMEOUT  -> stroke exceeded 120 s; show error briefly.
             */
            cal_result_t r = calibrate();
            /* Belt-and-suspenders: force-stop both axes in any exit. */
            set_axis_ns(AXIS_OFF);
            set_axis_ew(AXIS_OFF);

            lcd_clear();
            lcd_goto(0, 0);
            switch (r) {
                case CAL_OK:
                    lcd_print_padded("Cal done", 16);
                    lcd_goto(1, 0);
                    lcd_print("NS=");
                    lcd_print_secs_tenths((unsigned long)ns_stroke_ms);
                    lcd_print(" EW=");
                    lcd_print_secs_tenths((unsigned long)ew_stroke_ms);
                    break;
                case CAL_ABORTED:
                    lcd_print_padded("Cal aborted", 16);
                    break;
                case CAL_TIMEOUT:
                    lcd_print_padded("Cal timeout", 16);
                    lcd_goto(1, 0); lcd_print_padded("check wiring", 16);
                    break;
            }
            /* Hold result on screen for a moment so the user can see it. */
            delay_ms(2000);
            /* Cal ends at the retract endstop -> re-zero the integrator
             * and re-arm the tick clocks.  Then impute cal's thermal
             * cost: the motors ran ~3x stroke uninterrupted (blocking
             * cal, duty_tick blind), so force the cooldown lockout --
             * the padlock will show until the motors have rested. */
            position_reset();
            duty_force_cooldown();
            lcd_clear();
            idle_ticks = 0;
            prev_button = BTN_NONE;
            state = ST_IDLE;
            break;
        }

        case ST_BTEST: {
            /* Backlash characterization: prime FWD then pulse REV by
             * btest_pulse_ms, watching for visible motion.  The smallest
             * pulse_ms that produces motion is the gear backlash time.
             *
             * Axis select: N/S keys -> NS axis, E/W keys -> EW axis.
             * SET: run one prime+pulse cycle, then increment pulse_ms.
             * QUIT: exit to menu.
             *
             * Position tracker is not updated during the test sequence
             * (the prime FWD does move the actuator, but we re-arm
             * pos_last_tick_ms afterward so it's not credited weirdly). */
            const char *axis_lbl = (btest_axis == CAL_AXIS_NS) ? "NS" : "EW";

            lcd_goto(0, 0);
            lcd_print("Backlash ");
            lcd_print(axis_lbl);
            lcd_print("     ");   /* pad to 16 */

            lcd_goto(1, 0);
            lcd_print("Pulse:");
            lcd_putc('0' + (btest_pulse_ms / 100));
            lcd_putc('0' + ((btest_pulse_ms / 10) % 10));
            lcd_putc('0' + (btest_pulse_ms % 10));
            lcd_print(" ms     ");

            if (pressed == BTN_NORTH || pressed == BTN_SOUTH) {
                btest_axis = CAL_AXIS_NS;
            } else if (pressed == BTN_EAST || pressed == BTN_WEST) {
                btest_axis = CAL_AXIS_EW;
            } else if (pressed == BTN_SET) {
                /* Prime FWD 500 ms -- comfortably above expected backlash. */
                lcd_clear();
                lcd_goto(0, 0); lcd_print_padded("Priming FWD...", 16);
                cal_axis_set(btest_axis, AXIS_FWD);
                delay_ms(500);
                cal_axis_set(btest_axis, AXIS_OFF);
                delay_ms(300);

                /* REV pulse of pulse_ms. */
                lcd_goto(0, 0); lcd_print_padded("REV pulse...", 16);
                cal_axis_set(btest_axis, AXIS_REV);
                delay_ms(btest_pulse_ms);
                cal_axis_set(btest_axis, AXIS_OFF);
                delay_ms(500);

                btest_pulse_ms += 20;
                if (btest_pulse_ms > 999) btest_pulse_ms = 20;
                lcd_clear();
                pos_last_tick_ms = millis();   /* re-arm tick */
            } else if (pressed == BTN_QUIT) {
                cal_axis_set(btest_axis, AXIS_OFF);  /* belt + suspenders */
                lcd_clear();
                state = ST_MENU;
            }
            break;
        }

        case ST_JOG:
            jog_tick(pressed, curr_button,
                     (idle_ticks >= INACTIVITY_LIMIT) ? 1 : 0, &state);
            break;

        case ST_SETTINGS:
            settings_list_tick(pressed, &state);
            break;

        case ST_SETTINGS_EDIT:
            settings_edit_tick(pressed, &state);
            break;

        case ST_VERSION:
            lcd_goto(0, 0); lcd_print_padded(FIRMWARE_VERSION, 16);
            lcd_goto(1, 0); lcd_print_padded(build_date, 16);
            if (pressed == BTN_QUIT) {
                lcd_clear();
                state = ST_MENU;
            }
            break;
        }

        /* Duty-lockout indicator: padlock glyph (CGRAM 0) at the
         * bottom-right cell whenever either axis is duty-locked,
         * blank otherwise.  Overlaid after the state render so it's
         * visible on every screen via one code path. */
        lcd_goto(1, 15);
        lcd_putc((ns_duty_locked() || ew_duty_locked()) ? 0x00 : ' ');

        delay_ms(2);   /* was 50 (~150ms actual) -- crushed poll rate */
    }
}
