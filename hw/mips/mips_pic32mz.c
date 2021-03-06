/*
 * QEMU support for Microchip PIC32MZ microcontroller.
 *
 * Copyright (c) 2015 Serge Vakulenko
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Only 32-bit little endian mode supported. */
#include "config.h"
#if !defined TARGET_MIPS64 && !defined TARGET_WORDS_BIGENDIAN

#include "hw/i386/pc.h"
#include "hw/char/serial.h"
#include "hw/mips/cpudevs.h"
#include "sysemu/char.h"
#include "hw/loader.h"
#include "qemu/error-report.h"
#include "hw/empty_slot.h"
#include <termios.h>
#include <time.h>

#include "pic32mz.h"
#include "pic32_peripherals.h"

/* Hardware addresses */
#define PROGRAM_FLASH_START 0x1d000000
#define BOOT_FLASH_START    0x1fc00000
#define DATA_MEM_START      0x00000000
#define IO_MEM_START        0x1f800000

#define PROGRAM_FLASH_SIZE  (2*1024*1024)       // 2 Mbytes
#define BOOT_FLASH_SIZE     (64*1024)           // 64 kbytes
#define DATA_MEM_SIZE       (512*1024)          // 512 kbytes

#define TYPE_MIPS_PIC32     "mips-pic32mz"

/* decimal to BCD */
#define TOBCD(x)    (((x) / 10 * 16) + ((x) % 10))

/*
 * Board variants.
 */
enum {
    BOARD_WIFIRE,           /* chipKIT WiFire board */
    BOARD_MEBII,            /* Microchip MEB-II board */
    BOARD_EXPLORER16,       /* Microchip Explorer-16 board */
    BOARD_HMZ144,           /* Olimex HMZ144 board */
};

static const char *board_name[] = {
    "chipKIT WiFire",
    "Microchip MEB-II",
    "Microchip Explorer16",
    "Olimex HMZ144",
};

/*
 * Pointers to Flash memory contents.
 */
static char *prog_ptr;
static char *boot_ptr;

#define BOOTMEM(addr) ((uint32_t*) boot_ptr) [(addr & 0xffff) >> 2]

static void update_irq_status(pic32_t *s)
{
    /* Assume no interrupts pending. */
    int cause_ripl = 0;
    int vector = 0;
    CPUMIPSState *env = &s->cpu->env;
    int current_ripl = (env->CP0_Cause >> (CP0Ca_IP + 2)) & 0x3f;

    VALUE(INTSTAT) = 0;

    if ((VALUE(IFS0) & VALUE(IEC0)) ||
        (VALUE(IFS1) & VALUE(IEC1)) ||
        (VALUE(IFS2) & VALUE(IEC2)) ||
        (VALUE(IFS3) & VALUE(IEC3)) ||
        (VALUE(IFS4) & VALUE(IEC4)) ||
        (VALUE(IFS5) & VALUE(IEC5)))
    {
        /* Find the most prioritive pending interrupt,
         * it's vector and level. */
        int irq;
        for (irq=0; irq<=PIC32_IRQ_LAST; irq++) {
            int n = irq >> 5;

            if (((VALUE(IFS(n)) & VALUE(IEC(n))) >> (irq & 31)) & 1) {
                /* Interrupt is pending. */
                int level = VALUE(IPC(irq >> 2));
                level >>= 2 + (irq & 3) * 8;
                level &= 7;
                if (level > cause_ripl) {
                    vector = irq;
                    cause_ripl = level;
                }
            }
        }
        VALUE(INTSTAT) = vector | (cause_ripl << 8);
    }

    if (cause_ripl == current_ripl)
        return;

    if (qemu_loglevel_mask(CPU_LOG_INSTR))
        fprintf(qemu_logfile, "--- Priority level Cause.RIPL = %u\n",
            cause_ripl);

    /*
     * Modify Cause.RIPL field and take EIC interrupt.
     */
    env->CP0_Cause &= ~(0x3f << (CP0Ca_IP + 2));
    env->CP0_Cause |= cause_ripl << (CP0Ca_IP + 2);
    cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
}

/*
 * Set interrupt flag status
 */
static void irq_raise(pic32_t *s, int irq)
{
    if (VALUE(IFS(irq >> 5)) & (1 << (irq & 31)))
        return;

    VALUE(IFS(irq >> 5)) |= 1 << (irq & 31);
    update_irq_status(s);
}

/*
 * Clear interrupt flag status
 */
static void irq_clear(pic32_t *s, int irq)
{
    if (! (VALUE(IFS(irq >> 5)) & (1 << (irq & 31))))
        return;

    VALUE(IFS(irq >> 5)) &= ~(1 << (irq & 31));
    update_irq_status(s);
}

/*
 * Timer interrupt.
 */
static void pic32_timer_irq(CPUMIPSState *env, int raise)
{
    pic32_t *s = env->eic_context;

    if (raise) {
        if (qemu_loglevel_mask(CPU_LOG_INSTR))
            fprintf(qemu_logfile, "--- %08x: Timer interrupt\n",
                env->active_tc.PC);
        irq_raise(s, 0);
    } else {
        if (qemu_loglevel_mask(CPU_LOG_INSTR))
            fprintf(qemu_logfile, "--- Clear timer interrupt\n");
        irq_clear(s, 0);
    }
}

/*
 * Software interrupt.
 */
static void pic32_soft_irq(CPUMIPSState *env, int num)
{
    pic32_t *s = env->eic_context;

    if (qemu_loglevel_mask(CPU_LOG_INSTR))
        fprintf(qemu_logfile, "--- %08x: Soft interrupt %u\n",
            env->active_tc.PC, num);
    irq_raise(s, num + 1);
}

/*
 * Perform an assign/clear/set/invert operation.
 */
static inline unsigned write_op(int a, int b, int op)
{
    switch (op & 0xc) {
    case 0x0: a = b;   break;   // Assign
    case 0x4: a &= ~b; break;   // Clear
    case 0x8: a |= b;  break;   // Set
    case 0xc: a ^= b;  break;   // Invert
    }
    return a;
}

static void io_reset(pic32_t *s)
{
    int i;
    time_t now;
    struct tm *cl;

    /*
     * Prefetch controller.
     */
    VALUE(PRECON) = 0x00000007;

    /*
     * System controller.
     */
    s->syskey_unlock = 0;
    VALUE(CFGCON) = PIC32_CFGCON_ECC_DISWR | PIC32_CFGCON_TDOEN;
    VALUE(SYSKEY) = 0;
    VALUE(RCON)   = 0;
    VALUE(RSWRST) = 0;
    VALUE(OSCTUN) = 0;
    if (s->board_type == BOARD_HMZ144)
      VALUE(SPLLCON)   = 0x01630201;
    else
      VALUE(SPLLCON)= 0x01310201;
    VALUE(PB1DIV) = 0x00008801;
    VALUE(PB2DIV) = 0x00008801;
    VALUE(PB3DIV) = 0x00008801;
    VALUE(PB4DIV) = 0x00008801;
    VALUE(PB5DIV) = 0x00008801;
    VALUE(PB7DIV) = 0x00008800;
    VALUE(PB8DIV) = 0x00008801;

    /*
     * Real-Time Clock and Calendar.
     */
    VALUE(RTCCON) = 0;
    time(&now);
    cl = gmtime(&now);
    VALUE(RTCTIME) =
       TOBCD(cl->tm_sec) << PIC32_RTCTIME_SEC |
       TOBCD(cl->tm_min) << PIC32_RTCTIME_MIN |
       TOBCD(cl->tm_hour) << PIC32_RTCTIME_HOUR;
    VALUE(RTCDATE) =
       TOBCD(cl->tm_wday) |
       TOBCD(cl->tm_mday) << PIC32_RTCDATE_DAY |
       TOBCD(cl->tm_mon + 1) << PIC32_RTCDATE_MONTH |
       TOBCD(cl->tm_year + 1900 - 2000) << PIC32_RTCDATE_YEAR;

    /*
     * General purpose IO signals.
     * All pins are inputs, high, open drains and pullups disabled.
     * No interrupts on change.
     */
    VALUE(ANSELA) = 0xFFFF;             // Port A: analog select
    VALUE(TRISA) = 0xFFFF;              // Port A: mask of inputs
    VALUE(PORTA) = 0xFFCF;              // Port A: read inputs, write outputs
    VALUE(LATA)  = 0xFFFF;              // Port A: read/write outputs
    VALUE(ODCA)  = 0;                   // Port A: open drain configuration
    VALUE(CNPUA) = 0;                   // Input pin pull-up
    VALUE(CNPDA) = 0;                   // Input pin pull-down
    VALUE(CNCONA) = 0;                  // Interrupt-on-change control
    VALUE(CNENA) = 0;                   // Input change interrupt enable
    VALUE(CNSTATA) = 0;                 // Input change status

    VALUE(ANSELB) = 0xFFFF;             // Port B: analog select
    VALUE(TRISB) = 0xFFFF;              // Port B: mask of inputs
    VALUE(PORTB) = 0xFFFF;              // Port B: read inputs, write outputs
    if (s->board_type == BOARD_MEBII)
        VALUE(PORTB) ^= 1 << 12;        // Disable pin RB12 - button 1
    VALUE(LATB)  = 0xFFFF;              // Port B: read/write outputs
    VALUE(ODCB)  = 0;                   // Port B: open drain configuration
    VALUE(CNPUB) = 0;                   // Input pin pull-up
    VALUE(CNPDB) = 0;                   // Input pin pull-down
    VALUE(CNCONB) = 0;                  // Interrupt-on-change control
    VALUE(CNENB) = 0;                   // Input change interrupt enable
    VALUE(CNSTATB) = 0;                 // Input change status

    VALUE(ANSELC) = 0xFFFF;             // Port C: analog select
    VALUE(TRISC) = 0xFFFF;              // Port C: mask of inputs
    VALUE(PORTC) = 0xFFFF;              // Port C: read inputs, write outputs
    VALUE(LATC)  = 0xFFFF;              // Port C: read/write outputs
    if (s->board_type == BOARD_WIFIRE)
        VALUE(LATC) ^= 0x1000;          // Disable latc[15] for the chipKIT bootloader
    VALUE(ODCC)  = 0;                   // Port C: open drain configuration
    VALUE(CNPUC) = 0;                   // Input pin pull-up
    VALUE(CNPDC) = 0;                   // Input pin pull-down
    VALUE(CNCONC) = 0;                  // Interrupt-on-change control
    VALUE(CNENC) = 0;                   // Input change interrupt enable
    VALUE(CNSTATC) = 0;                 // Input change status

    VALUE(ANSELD) = 0xFFFF;             // Port D: analog select
    VALUE(TRISD) = 0xFFFF;              // Port D: mask of inputs
    VALUE(PORTD) = 0xFFFF;              // Port D: read inputs, write outputs
    VALUE(LATD)  = 0xFFFF;              // Port D: read/write outputs
    VALUE(ODCD)  = 0;                   // Port D: open drain configuration
    VALUE(CNPUD) = 0;                   // Input pin pull-up
    VALUE(CNPDD) = 0;                   // Input pin pull-down
    VALUE(CNCOND) = 0;                  // Interrupt-on-change control
    VALUE(CNEND) = 0;                   // Input change interrupt enable
    VALUE(CNSTATD) = 0;                 // Input change status

    VALUE(ANSELE) = 0xFFFF;             // Port E: analog select
    VALUE(TRISE) = 0xFFFF;              // Port E: mask of inputs
    VALUE(PORTE) = 0xFFFF;              // Port E: read inputs, write outputs
    VALUE(LATE)  = 0xFFFF;              // Port E: read/write outputs
    VALUE(ODCE)  = 0;                   // Port E: open drain configuration
    VALUE(CNPUE) = 0;                   // Input pin pull-up
    VALUE(CNPDE) = 0;                   // Input pin pull-down
    VALUE(CNCONE) = 0;                  // Interrupt-on-change control
    VALUE(CNENE) = 0;                   // Input change interrupt enable
    VALUE(CNSTATE) = 0;                 // Input change status

    VALUE(ANSELF) = 0xFFFF;             // Port F: analog select
    VALUE(TRISF) = 0xFFFF;              // Port F: mask of inputs
    VALUE(PORTF) = 0xFFFF;              // Port F: read inputs, write outputs
    VALUE(LATF)  = 0xFFFF;              // Port F: read/write outputs
    VALUE(ODCF)  = 0;                   // Port F: open drain configuration
    VALUE(CNPUF) = 0;                   // Input pin pull-up
    VALUE(CNPDF) = 0;                   // Input pin pull-down
    VALUE(CNCONF) = 0;                  // Interrupt-on-change control
    VALUE(CNENF) = 0;                   // Input change interrupt enable
    VALUE(CNSTATF) = 0;                 // Input change status

    VALUE(ANSELG) = 0xFFFF;             // Port G: analog select
    VALUE(TRISG) = 0xFFFF;              // Port G: mask of inputs
    VALUE(PORTG) = 0xFFFF;              // Port G: read inputs, write outputs
    VALUE(LATG)  = 0xFFFF;              // Port G: read/write outputs
    VALUE(ODCG)  = 0;                   // Port G: open drain configuration
    VALUE(CNPUG) = 0;                   // Input pin pull-up
    VALUE(CNPDG) = 0;                   // Input pin pull-down
    VALUE(CNCONG) = 0;                  // Interrupt-on-change control
    VALUE(CNENG) = 0;                   // Input change interrupt enable
    VALUE(CNSTATG) = 0;                 // Input change status

    VALUE(ANSELH) = 0xFFFF;             // Port H: analog select
    VALUE(TRISH) = 0xFFFF;              // Port H: mask of inputs
    VALUE(PORTH) = 0xFFFF;              // Port H: read inputs, write outputs
    VALUE(LATH)  = 0xFFFF;              // Port H: read/write outputs
    VALUE(ODCH)  = 0;                   // Port H: open drain configuration
    VALUE(CNPUH) = 0;                   // Input pin pull-up
    VALUE(CNPDH) = 0;                   // Input pin pull-down
    VALUE(CNCONH) = 0;                  // Interrupt-on-change control
    VALUE(CNENH) = 0;                   // Input change interrupt enable
    VALUE(CNSTATH) = 0;                 // Input change status

    VALUE(ANSELJ) = 0xFFFF;             // Port J: analog select
    VALUE(TRISJ) = 0xFFFF;              // Port J: mask of inputs
    VALUE(PORTJ) = 0xFFFF;              // Port J: read inputs, write outputs
    VALUE(LATJ)  = 0xFFFF;              // Port J: read/write outputs
    VALUE(ODCJ)  = 0;                   // Port J: open drain configuration
    VALUE(CNPUJ) = 0;                   // Input pin pull-up
    VALUE(CNPDJ) = 0;                   // Input pin pull-down
    VALUE(CNCONJ) = 0;                  // Interrupt-on-change control
    VALUE(CNENJ) = 0;                   // Input change interrupt enable
    VALUE(CNSTATJ) = 0;                 // Input change status

    VALUE(TRISK) = 0xFFFF;              // Port K: mask of inputs
    VALUE(PORTK) = 0xFFFF;              // Port K: read inputs, write outputs
    VALUE(LATK)  = 0xFFFF;              // Port K: read/write outputs
    VALUE(ODCK)  = 0;                   // Port K: open drain configuration
    VALUE(CNPUK) = 0;                   // Input pin pull-up
    VALUE(CNPDK) = 0;                   // Input pin pull-down
    VALUE(CNCONK) = 0;                  // Interrupt-on-change control
    VALUE(CNENK) = 0;                   // Input change interrupt enable
    VALUE(CNSTATK) = 0;                 // Input change status

    /*
     * Reset UARTs.
     */
    VALUE(U1MODE)  = 0;
    VALUE(U1STA)   = PIC32_USTA_RIDLE | PIC32_USTA_TRMT;
    VALUE(U1TXREG) = 0;
    VALUE(U1RXREG) = 0;
    VALUE(U1BRG)   = 0;
    VALUE(U2MODE)  = 0;
    VALUE(U2STA)   = PIC32_USTA_RIDLE | PIC32_USTA_TRMT;
    VALUE(U2TXREG) = 0;
    VALUE(U2RXREG) = 0;
    VALUE(U2BRG)   = 0;
    VALUE(U3MODE)  = 0;
    VALUE(U3STA)   = PIC32_USTA_RIDLE | PIC32_USTA_TRMT;
    VALUE(U3TXREG) = 0;
    VALUE(U3RXREG) = 0;
    VALUE(U3BRG)   = 0;
    VALUE(U4MODE)  = 0;
    VALUE(U4STA)   = PIC32_USTA_RIDLE | PIC32_USTA_TRMT;
    VALUE(U4TXREG) = 0;
    VALUE(U4RXREG) = 0;
    VALUE(U4BRG)   = 0;
    VALUE(U5MODE)  = 0;
    VALUE(U5STA)   = PIC32_USTA_RIDLE | PIC32_USTA_TRMT;
    VALUE(U5TXREG) = 0;
    VALUE(U5RXREG) = 0;
    VALUE(U5BRG)   = 0;
    VALUE(U6MODE)  = 0;
    VALUE(U6STA)   = PIC32_USTA_RIDLE | PIC32_USTA_TRMT;
    VALUE(U6TXREG) = 0;
    VALUE(U6RXREG) = 0;
    VALUE(U6BRG)   = 0;

    /*
     * Reset SPI.
     */
    VALUE(SPI1CON)  = 0;
    VALUE(SPI1STAT) = PIC32_SPISTAT_SPITBE;     // Transmit buffer is empty
    VALUE(SPI1BRG)  = 0;

    VALUE(SPI2CON)  = 0;
    VALUE(SPI2STAT) = PIC32_SPISTAT_SPITBE;     // Transmit buffer is empty
    VALUE(SPI2BRG)  = 0;

    VALUE(SPI3CON)  = 0;
    VALUE(SPI3STAT) = PIC32_SPISTAT_SPITBE;     // Transmit buffer is empty
    VALUE(SPI3BRG)  = 0;

    VALUE(SPI4CON)  = 0;
    VALUE(SPI4STAT) = PIC32_SPISTAT_SPITBE;     // Transmit buffer is empty
    VALUE(SPI4BRG)  = 0;

    VALUE(SPI5CON)  = 0;
    VALUE(SPI5STAT) = PIC32_SPISTAT_SPITBE;     // Transmit buffer is empty
    VALUE(SPI5BRG)  = 0;

    VALUE(SPI6CON)  = 0;
    VALUE(SPI6STAT) = PIC32_SPISTAT_SPITBE;     // Transmit buffer is empty
    VALUE(SPI6BRG)  = 0;

    VALUE(SPI1CON2) = 0;
    VALUE(SPI2CON2) = 0;
    VALUE(SPI3CON2) = 0;
    VALUE(SPI4CON2) = 0;
    VALUE(SPI5CON2) = 0;
    VALUE(SPI6CON2) = 0;

    for (i=0; i<NUM_SPI; i++) {
        s->spi[i].rfifo = 0;
        s->spi[i].wfifo = 0;
    }

    /*
     * Reset timers.
     */
    VALUE(T1CON)    = 0;
    VALUE(TMR1)     = 0;
    VALUE(PR1)      = 0xffff;
    VALUE(T2CON)    = 0;
    VALUE(TMR2)     = 0;
    VALUE(PR2)      = 0xffff;
    VALUE(T3CON)    = 0;
    VALUE(TMR3)     = 0;
    VALUE(PR3)      = 0xffff;
    VALUE(T4CON)    = 0;
    VALUE(TMR4)     = 0;
    VALUE(PR4)      = 0xffff;
    VALUE(T5CON)    = 0;
    VALUE(TMR5)     = 0;
    VALUE(PR5)      = 0xffff;
    VALUE(T6CON)    = 0;
    VALUE(TMR6)     = 0;
    VALUE(PR6)      = 0xffff;
    VALUE(T7CON)    = 0;
    VALUE(TMR7)     = 0;
    VALUE(PR7)      = 0xffff;
    VALUE(T8CON)    = 0;
    VALUE(TMR8)     = 0;
    VALUE(PR8)      = 0xffff;
    VALUE(T9CON)    = 0;
    VALUE(TMR9)     = 0;
    VALUE(PR9)      = 0xffff;

    /*
     * Reset Ethernet.
     */
    VALUE(ETHCON1)      = 0;        // Control 1
    VALUE(ETHCON2)      = 0;        // Control 2: RX data buffer size
    VALUE(ETHTXST)      = 0;        // Tx descriptor start address
    VALUE(ETHRXST)      = 0;        // Rx descriptor start address
    VALUE(ETHHT0)       = 0;        // Hash tasble 0
    VALUE(ETHHT1)       = 0;        // Hash tasble 1
    VALUE(ETHPMM0)      = 0;        // Pattern match mask 0
    VALUE(ETHPMM1)      = 0;        // Pattern match mask 1
    VALUE(ETHPMCS)      = 0;        // Pattern match checksum
    VALUE(ETHPMO)       = 0;        // Pattern match offset
    VALUE(ETHRXFC)      = 0;        // Receive filter configuration
    VALUE(ETHRXWM)      = 0;        // Receive watermarks
    VALUE(ETHIEN)       = 0;        // Interrupt enable
    VALUE(ETHIRQ)       = 0;        // Interrupt request
    VALUE(ETHSTAT)      = 0;        // Status
    VALUE(ETHRXOVFLOW)  = 0;        // Receive overflow statistics
    VALUE(ETHFRMTXOK)   = 0;        // Frames transmitted OK statistics
    VALUE(ETHSCOLFRM)   = 0;        // Single collision frames statistics
    VALUE(ETHMCOLFRM)   = 0;        // Multiple collision frames statistics
    VALUE(ETHFRMRXOK)   = 0;        // Frames received OK statistics
    VALUE(ETHFCSERR)    = 0;        // Frame check sequence error statistics
    VALUE(ETHALGNERR)   = 0;        // Alignment errors statistics
    VALUE(EMAC1CFG1)    = 0x800d;   // MAC configuration 1
    VALUE(EMAC1CFG2)    = 0x4082;   // MAC configuration 2
    VALUE(EMAC1IPGT)    = 0x0012;   // MAC back-to-back interpacket gap
    VALUE(EMAC1IPGR)    = 0x0c12;   // MAC non-back-to-back interpacket gap
    VALUE(EMAC1CLRT)    = 0x370f;   // MAC collision window/retry limit
    VALUE(EMAC1MAXF)    = 0x05ee;   // MAC maximum frame length
    VALUE(EMAC1SUPP)    = 0x1000;   // MAC PHY support
    VALUE(EMAC1TEST)    = 0;        // MAC test
    VALUE(EMAC1MCFG)    = 0x0020;   // MII configuration
    VALUE(EMAC1MCMD)    = 0;        // MII command
    VALUE(EMAC1MADR)    = 0x0100;   // MII address
    VALUE(EMAC1MWTD)    = 0;        // MII write data
    VALUE(EMAC1MRDD)    = 0;        // MII read data
    VALUE(EMAC1MIND)    = 0;        // MII indicators
    VALUE(EMAC1SA0)     = 0x79c1;   // MAC station address 0
    VALUE(EMAC1SA1)     = 0xcbc0;   // MAC station address 1
    VALUE(EMAC1SA2)     = 0x1e00;   // MAC station address 2

    /*
     * Reset USB.
     */
    VALUE(USBCSR0)      = 0x2000;
    VALUE(USBCSR1)      = 0x00ff0000;
    VALUE(USBCSR2)      = 0x060000fe;
    VALUE(USBCSR3)      = 0;
    VALUE(USBIENCSR0)   = 0;
    VALUE(USBIENCSR1)   = 0;
    VALUE(USBIENCSR2)   = 0;
    VALUE(USBIENCSR3)   = 0;
    VALUE(USBFIFO0)     = 0;
    VALUE(USBFIFO1)     = 0;
    VALUE(USBFIFO2)     = 0;
    VALUE(USBFIFO3)     = 0;
    VALUE(USBFIFO4)     = 0;
    VALUE(USBFIFO5)     = 0;
    VALUE(USBFIFO6)     = 0;
    VALUE(USBFIFO7)     = 0;
    VALUE(USBOTG)       = 0x0080;
    VALUE(USBFIFOA)     = 0;
    VALUE(USBHWVER)     = 0x0800;
    VALUE(USBINFO)      = 0x3C5C8C77;
    VALUE(USBEOFRST)    = 0x00727780;
    VALUE(USBE0TXA)     = 0;
    VALUE(USBE0RXA)     = 0;
    VALUE(USBE1TXA)     = 0;
    VALUE(USBE1RXA)     = 0;
    VALUE(USBE2TXA)     = 0;
    VALUE(USBE2RXA)     = 0;
    VALUE(USBE3TXA)     = 0;
    VALUE(USBE3RXA)     = 0;
    VALUE(USBE4TXA)     = 0;
    VALUE(USBE4RXA)     = 0;
    VALUE(USBE5TXA)     = 0;
    VALUE(USBE5RXA)     = 0;
    VALUE(USBE6TXA)     = 0;
    VALUE(USBE6RXA)     = 0;
    VALUE(USBE7TXA)     = 0;
    VALUE(USBE7RXA)     = 0;
    VALUE(USBE0CSR0)    = 0;
    VALUE(USBE0CSR2)    = 0;
    VALUE(USBE0CSR3)    = 0;
    VALUE(USBE1CSR0)    = 0;
    VALUE(USBE1CSR1)    = 0;
    VALUE(USBE1CSR2)    = 0;
    VALUE(USBE1CSR3)    = 0;
    VALUE(USBE2CSR0)    = 0;
    VALUE(USBE2CSR1)    = 0;
    VALUE(USBE2CSR2)    = 0;
    VALUE(USBE2CSR3)    = 0;
    VALUE(USBE3CSR0)    = 0;
    VALUE(USBE3CSR1)    = 0;
    VALUE(USBE3CSR2)    = 0;
    VALUE(USBE3CSR3)    = 0;
    VALUE(USBE4CSR0)    = 0;
    VALUE(USBE4CSR1)    = 0;
    VALUE(USBE4CSR2)    = 0;
    VALUE(USBE4CSR3)    = 0;
    VALUE(USBE5CSR0)    = 0;
    VALUE(USBE5CSR1)    = 0;
    VALUE(USBE5CSR2)    = 0;
    VALUE(USBE5CSR3)    = 0;
    VALUE(USBE6CSR0)    = 0;
    VALUE(USBE6CSR1)    = 0;
    VALUE(USBE6CSR2)    = 0;
    VALUE(USBE6CSR3)    = 0;
    VALUE(USBE7CSR0)    = 0;
    VALUE(USBE7CSR1)    = 0;
    VALUE(USBE7CSR2)    = 0;
    VALUE(USBE7CSR3)    = 0;
    VALUE(USBDMAINT)    = 0;
    VALUE(USBDMA1C)     = 0;
    VALUE(USBDMA1A)     = 0;
    VALUE(USBDMA1N)     = 0;
    VALUE(USBDMA2C)     = 0;
    VALUE(USBDMA2A)     = 0;
    VALUE(USBDMA2N)     = 0;
    VALUE(USBDMA3C)     = 0;
    VALUE(USBDMA3A)     = 0;
    VALUE(USBDMA3N)     = 0;
    VALUE(USBDMA4C)     = 0;
    VALUE(USBDMA4A)     = 0;
    VALUE(USBDMA4N)     = 0;
    VALUE(USBDMA5C)     = 0;
    VALUE(USBDMA5A)     = 0;
    VALUE(USBDMA5N)     = 0;
    VALUE(USBDMA6C)     = 0;
    VALUE(USBDMA6A)     = 0;
    VALUE(USBDMA6N)     = 0;
    VALUE(USBDMA7C)     = 0;
    VALUE(USBDMA7A)     = 0;
    VALUE(USBDMA7N)     = 0;
    VALUE(USBDMA8C)     = 0;
    VALUE(USBDMA8A)     = 0;
    VALUE(USBDMA8N)     = 0;
    VALUE(USBE1RPC)     = 0;
    VALUE(USBE2RPC)     = 0;
    VALUE(USBE3RPC)     = 0;
    VALUE(USBE4RPC)     = 0;
    VALUE(USBE5RPC)     = 0;
    VALUE(USBE6RPC)     = 0;
    VALUE(USBE7RPC)     = 0;
    VALUE(USBDPBFD)     = 0;
    VALUE(USBTMCON1)    = 0x05E64074;
    VALUE(USBTMCON2)    = 0;
    VALUE(USBLPMR1)     = 0;
    VALUE(USBLMPR2)     = 0;
}

static unsigned io_read32(pic32_t *s, unsigned offset, const char **namep)
{
    unsigned *bufp = &VALUE(offset);

    switch (offset) {
    /*-------------------------------------------------------------------------
     * Interrupt controller registers.
     */
    STORAGE(INTCON); break;     // Interrupt Control
    STORAGE(INTSTAT); break;    // Interrupt Status
    STORAGE(IFS0); break;       // IFS(0..2) - Interrupt Flag Status
    STORAGE(IFS1); break;
    STORAGE(IFS2); break;
    STORAGE(IFS3); break;
    STORAGE(IFS4); break;
    STORAGE(IFS5); break;
    STORAGE(IEC0); break;       // IEC(0..2) - Interrupt Enable Control
    STORAGE(IEC1); break;
    STORAGE(IEC2); break;
    STORAGE(IEC3); break;
    STORAGE(IEC4); break;
    STORAGE(IEC5); break;

    // IPC(0..11) - Interrupt Priority Control
    STORAGE(IPC0); break;       STORAGE(IPC1); break;
    STORAGE(IPC2); break;       STORAGE(IPC3); break;
    STORAGE(IPC4); break;       STORAGE(IPC5); break;
    STORAGE(IPC6); break;       STORAGE(IPC7); break;
    STORAGE(IPC8); break;       STORAGE(IPC9); break;
    STORAGE(IPC10); break;      STORAGE(IPC11); break;
    STORAGE(IPC12); break;      STORAGE(IPC13); break;
    STORAGE(IPC14); break;      STORAGE(IPC15); break;
    STORAGE(IPC16); break;      STORAGE(IPC17); break;
    STORAGE(IPC18); break;      STORAGE(IPC19); break;
    STORAGE(IPC20); break;      STORAGE(IPC21); break;
    STORAGE(IPC22); break;      STORAGE(IPC23); break;
    STORAGE(IPC24); break;      STORAGE(IPC25); break;
    STORAGE(IPC26); break;      STORAGE(IPC27); break;
    STORAGE(IPC28); break;      STORAGE(IPC29); break;
    STORAGE(IPC30); break;      STORAGE(IPC31); break;
    STORAGE(IPC32); break;      STORAGE(IPC33); break;
    STORAGE(IPC34); break;      STORAGE(IPC35); break;
    STORAGE(IPC36); break;      STORAGE(IPC37); break;
    STORAGE(IPC38); break;      STORAGE(IPC39); break;
    STORAGE(IPC40); break;      STORAGE(IPC41); break;
    STORAGE(IPC42); break;      STORAGE(IPC43); break;
    STORAGE(IPC44); break;      STORAGE(IPC45); break;
    STORAGE(IPC46); break;      STORAGE(IPC47); break;

    // OFF000..OFF190 - Interrupt Vector Address Offset
    STORAGE(OFF(0)); break;     STORAGE(OFF(1)); break;
    STORAGE(OFF(2)); break;     STORAGE(OFF(3)); break;
    STORAGE(OFF(4)); break;     STORAGE(OFF(5)); break;
    STORAGE(OFF(6)); break;     STORAGE(OFF(7)); break;
    STORAGE(OFF(8)); break;     STORAGE(OFF(9)); break;
    STORAGE(OFF(10)); break;    STORAGE(OFF(11)); break;
    STORAGE(OFF(12)); break;    STORAGE(OFF(13)); break;
    STORAGE(OFF(14)); break;    STORAGE(OFF(15)); break;
    STORAGE(OFF(16)); break;    STORAGE(OFF(17)); break;
    STORAGE(OFF(18)); break;    STORAGE(OFF(19)); break;
    STORAGE(OFF(20)); break;    STORAGE(OFF(21)); break;
    STORAGE(OFF(22)); break;    STORAGE(OFF(23)); break;
    STORAGE(OFF(24)); break;    STORAGE(OFF(25)); break;
    STORAGE(OFF(26)); break;    STORAGE(OFF(27)); break;
    STORAGE(OFF(28)); break;    STORAGE(OFF(29)); break;
    STORAGE(OFF(30)); break;    STORAGE(OFF(31)); break;
    STORAGE(OFF(32)); break;    STORAGE(OFF(33)); break;
    STORAGE(OFF(34)); break;    STORAGE(OFF(35)); break;
    STORAGE(OFF(36)); break;    STORAGE(OFF(37)); break;
    STORAGE(OFF(38)); break;    STORAGE(OFF(39)); break;
    STORAGE(OFF(40)); break;    STORAGE(OFF(41)); break;
    STORAGE(OFF(42)); break;    STORAGE(OFF(43)); break;
    STORAGE(OFF(44)); break;    STORAGE(OFF(45)); break;
    STORAGE(OFF(46)); break;    STORAGE(OFF(47)); break;
    STORAGE(OFF(48)); break;    STORAGE(OFF(49)); break;
    STORAGE(OFF(50)); break;    STORAGE(OFF(51)); break;
    STORAGE(OFF(52)); break;    STORAGE(OFF(53)); break;
    STORAGE(OFF(54)); break;    STORAGE(OFF(55)); break;
    STORAGE(OFF(56)); break;    STORAGE(OFF(57)); break;
    STORAGE(OFF(58)); break;    STORAGE(OFF(59)); break;
    STORAGE(OFF(60)); break;    STORAGE(OFF(61)); break;
    STORAGE(OFF(62)); break;    STORAGE(OFF(63)); break;
    STORAGE(OFF(64)); break;    STORAGE(OFF(65)); break;
    STORAGE(OFF(66)); break;    STORAGE(OFF(67)); break;
    STORAGE(OFF(68)); break;    STORAGE(OFF(69)); break;
    STORAGE(OFF(70)); break;    STORAGE(OFF(71)); break;
    STORAGE(OFF(72)); break;    STORAGE(OFF(73)); break;
    STORAGE(OFF(74)); break;    STORAGE(OFF(75)); break;
    STORAGE(OFF(76)); break;    STORAGE(OFF(77)); break;
    STORAGE(OFF(78)); break;    STORAGE(OFF(79)); break;
    STORAGE(OFF(80)); break;    STORAGE(OFF(81)); break;
    STORAGE(OFF(82)); break;    STORAGE(OFF(83)); break;
    STORAGE(OFF(84)); break;    STORAGE(OFF(85)); break;
    STORAGE(OFF(86)); break;    STORAGE(OFF(87)); break;
    STORAGE(OFF(88)); break;    STORAGE(OFF(89)); break;
    STORAGE(OFF(90)); break;    STORAGE(OFF(91)); break;
    STORAGE(OFF(92)); break;    STORAGE(OFF(93)); break;
    STORAGE(OFF(94)); break;    STORAGE(OFF(95)); break;
    STORAGE(OFF(96)); break;    STORAGE(OFF(97)); break;
    STORAGE(OFF(98)); break;    STORAGE(OFF(99)); break;
    STORAGE(OFF(100)); break;   STORAGE(OFF(101)); break;
    STORAGE(OFF(102)); break;   STORAGE(OFF(103)); break;
    STORAGE(OFF(104)); break;   STORAGE(OFF(105)); break;
    STORAGE(OFF(106)); break;   STORAGE(OFF(107)); break;
    STORAGE(OFF(108)); break;   STORAGE(OFF(109)); break;
    STORAGE(OFF(110)); break;   STORAGE(OFF(111)); break;
    STORAGE(OFF(112)); break;   STORAGE(OFF(113)); break;
    STORAGE(OFF(114)); break;   STORAGE(OFF(115)); break;
    STORAGE(OFF(116)); break;   STORAGE(OFF(117)); break;
    STORAGE(OFF(118)); break;   STORAGE(OFF(119)); break;
    STORAGE(OFF(120)); break;   STORAGE(OFF(121)); break;
    STORAGE(OFF(122)); break;   STORAGE(OFF(123)); break;
    STORAGE(OFF(124)); break;   STORAGE(OFF(125)); break;
    STORAGE(OFF(126)); break;   STORAGE(OFF(127)); break;
    STORAGE(OFF(128)); break;   STORAGE(OFF(129)); break;
    STORAGE(OFF(130)); break;   STORAGE(OFF(131)); break;
    STORAGE(OFF(132)); break;   STORAGE(OFF(133)); break;
    STORAGE(OFF(134)); break;   STORAGE(OFF(135)); break;
    STORAGE(OFF(136)); break;   STORAGE(OFF(137)); break;
    STORAGE(OFF(138)); break;   STORAGE(OFF(139)); break;
    STORAGE(OFF(140)); break;   STORAGE(OFF(141)); break;
    STORAGE(OFF(142)); break;   STORAGE(OFF(143)); break;
    STORAGE(OFF(144)); break;   STORAGE(OFF(145)); break;
    STORAGE(OFF(146)); break;   STORAGE(OFF(147)); break;
    STORAGE(OFF(148)); break;   STORAGE(OFF(149)); break;
    STORAGE(OFF(150)); break;   STORAGE(OFF(151)); break;
    STORAGE(OFF(152)); break;   STORAGE(OFF(153)); break;
    STORAGE(OFF(154)); break;   STORAGE(OFF(155)); break;
    STORAGE(OFF(156)); break;   STORAGE(OFF(157)); break;
    STORAGE(OFF(158)); break;   STORAGE(OFF(159)); break;
    STORAGE(OFF(160)); break;   STORAGE(OFF(161)); break;
    STORAGE(OFF(162)); break;   STORAGE(OFF(163)); break;
    STORAGE(OFF(164)); break;   STORAGE(OFF(165)); break;
    STORAGE(OFF(166)); break;   STORAGE(OFF(167)); break;
    STORAGE(OFF(168)); break;   STORAGE(OFF(169)); break;
    STORAGE(OFF(170)); break;   STORAGE(OFF(171)); break;
    STORAGE(OFF(172)); break;   STORAGE(OFF(173)); break;
    STORAGE(OFF(174)); break;   STORAGE(OFF(175)); break;
    STORAGE(OFF(176)); break;   STORAGE(OFF(177)); break;
    STORAGE(OFF(178)); break;   STORAGE(OFF(179)); break;
    STORAGE(OFF(180)); break;   STORAGE(OFF(181)); break;
    STORAGE(OFF(182)); break;   STORAGE(OFF(183)); break;
    STORAGE(OFF(184)); break;   STORAGE(OFF(185)); break;
    STORAGE(OFF(186)); break;   STORAGE(OFF(187)); break;
    STORAGE(OFF(188)); break;   STORAGE(OFF(189)); break;
    STORAGE(OFF(190)); break;

    /*-------------------------------------------------------------------------
     * Prefetch controller.
     */
    STORAGE(PRECON); break;     // Prefetch Control
    STORAGE(PRESTAT); break;    // Prefetch Status

    /*-------------------------------------------------------------------------
     * System controller.
     */
    STORAGE(CFGCON); break;     // Configuration Control
    STORAGE(DEVID); break;      // Device Identifier
    STORAGE(SYSKEY); break;     // System Key
    STORAGE(RCON); break;       // Reset Control
    STORAGE(RSWRST);            // Software Reset
        if ((VALUE(RSWRST) & 1) && s->stop_on_reset) {
            exit(0);
        }
        break;
    STORAGE(OSCCON); break;     // Oscillator Control
    STORAGE(OSCTUN); break;     // Oscillator Tuning
    STORAGE(SPLLCON); break;    // System PLL Control
    STORAGE(REFO1CON); break;
    STORAGE(REFO1CONCLR); *bufp = 0; break;
    STORAGE(REFO1CONSET); *bufp = 0; break;
    STORAGE(REFO1CONINV); *bufp = 0; break;
    STORAGE(REFO2CON); break;
    STORAGE(REFO2CONCLR); *bufp = 0; break;
    STORAGE(REFO2CONSET); *bufp = 0; break;
    STORAGE(REFO2CONINV); *bufp = 0; break;
    STORAGE(REFO3CON); break;
    STORAGE(REFO3CONCLR); *bufp = 0; break;
    STORAGE(REFO3CONSET); *bufp = 0; break;
    STORAGE(REFO3CONINV); *bufp = 0; break;
    STORAGE(REFO4CON); break;
    STORAGE(REFO4CONCLR); *bufp = 0; break;
    STORAGE(REFO4CONSET); *bufp = 0; break;
    STORAGE(REFO4CONINV); *bufp = 0; break;
    STORAGE(PB1DIV); break;     // Peripheral bus 1 divisor
    STORAGE(PB1DIVCLR); *bufp = 0; break;
    STORAGE(PB1DIVSET); *bufp = 0; break;
    STORAGE(PB1DIVINV); *bufp = 0; break;
    STORAGE(PB2DIV); break;     // Peripheral bus 2 divisor
    STORAGE(PB2DIVCLR); *bufp = 0; break;
    STORAGE(PB2DIVSET); *bufp = 0; break;
    STORAGE(PB2DIVINV); *bufp = 0; break;
    STORAGE(PB3DIV); break;     // Peripheral bus 3 divisor
    STORAGE(PB3DIVCLR); *bufp = 0; break;
    STORAGE(PB3DIVSET); *bufp = 0; break;
    STORAGE(PB3DIVINV); *bufp = 0; break;
    STORAGE(PB4DIV); break;     // Peripheral bus 4 divisor
    STORAGE(PB4DIVCLR); *bufp = 0; break;
    STORAGE(PB4DIVSET); *bufp = 0; break;
    STORAGE(PB4DIVINV); *bufp = 0; break;
    STORAGE(PB5DIV); break;     // Peripheral bus 5 divisor
    STORAGE(PB5DIVCLR); *bufp = 0; break;
    STORAGE(PB5DIVSET); *bufp = 0; break;
    STORAGE(PB5DIVINV); *bufp = 0; break;
    STORAGE(PB7DIV); break;     // Peripheral bus 7 divisor
    STORAGE(PB7DIVCLR); *bufp = 0; break;
    STORAGE(PB7DIVSET); *bufp = 0; break;
    STORAGE(PB7DIVINV); *bufp = 0; break;
    STORAGE(PB8DIV); break;     // Peripheral bus 8 divisor
    STORAGE(PB8DIVCLR); *bufp = 0; break;
    STORAGE(PB8DIVSET); *bufp = 0; break;
    STORAGE(PB8DIVINV); *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * Peripheral port select registers: input.
     */
    STORAGE(INT1R); break;
    STORAGE(INT2R); break;
    STORAGE(INT3R); break;
    STORAGE(INT4R); break;
    STORAGE(T2CKR); break;
    STORAGE(T3CKR); break;
    STORAGE(T4CKR); break;
    STORAGE(T5CKR); break;
    STORAGE(T6CKR); break;
    STORAGE(T7CKR); break;
    STORAGE(T8CKR); break;
    STORAGE(T9CKR); break;
    STORAGE(IC1R); break;
    STORAGE(IC2R); break;
    STORAGE(IC3R); break;
    STORAGE(IC4R); break;
    STORAGE(IC5R); break;
    STORAGE(IC6R); break;
    STORAGE(IC7R); break;
    STORAGE(IC8R); break;
    STORAGE(IC9R); break;
    STORAGE(OCFAR); break;
    STORAGE(U1RXR); break;
    STORAGE(U1CTSR); break;
    STORAGE(U2RXR); break;
    STORAGE(U2CTSR); break;
    STORAGE(U3RXR); break;
    STORAGE(U3CTSR); break;
    STORAGE(U4RXR); break;
    STORAGE(U4CTSR); break;
    STORAGE(U5RXR); break;
    STORAGE(U5CTSR); break;
    STORAGE(U6RXR); break;
    STORAGE(U6CTSR); break;
    STORAGE(SDI1R); break;
    STORAGE(SS1R); break;
    STORAGE(SDI2R); break;
    STORAGE(SS2R); break;
    STORAGE(SDI3R); break;
    STORAGE(SS3R); break;
    STORAGE(SDI4R); break;
    STORAGE(SS4R); break;
    STORAGE(SDI5R); break;
    STORAGE(SS5R); break;
    STORAGE(SDI6R); break;
    STORAGE(SS6R); break;
    STORAGE(C1RXR); break;
    STORAGE(C2RXR); break;
    STORAGE(REFCLKI1R); break;
    STORAGE(REFCLKI3R); break;
    STORAGE(REFCLKI4R); break;

    /*-------------------------------------------------------------------------
     * Peripheral port select registers: output.
     */
    STORAGE(RPA14R); break;
    STORAGE(RPA15R); break;
    STORAGE(RPB0R); break;
    STORAGE(RPB1R); break;
    STORAGE(RPB2R); break;
    STORAGE(RPB3R); break;
    STORAGE(RPB5R); break;
    STORAGE(RPB6R); break;
    STORAGE(RPB7R); break;
    STORAGE(RPB8R); break;
    STORAGE(RPB9R); break;
    STORAGE(RPB10R); break;
    STORAGE(RPB14R); break;
    STORAGE(RPB15R); break;
    STORAGE(RPC1R); break;
    STORAGE(RPC2R); break;
    STORAGE(RPC3R); break;
    STORAGE(RPC4R); break;
    STORAGE(RPC13R); break;
    STORAGE(RPC14R); break;
    STORAGE(RPD0R); break;
    STORAGE(RPD1R); break;
    STORAGE(RPD2R); break;
    STORAGE(RPD3R); break;
    STORAGE(RPD4R); break;
    STORAGE(RPD5R); break;
    STORAGE(RPD6R); break;
    STORAGE(RPD7R); break;
    STORAGE(RPD9R); break;
    STORAGE(RPD10R); break;
    STORAGE(RPD11R); break;
    STORAGE(RPD12R); break;
    STORAGE(RPD14R); break;
    STORAGE(RPD15R); break;
    STORAGE(RPE3R); break;
    STORAGE(RPE5R); break;
    STORAGE(RPE8R); break;
    STORAGE(RPE9R); break;
    STORAGE(RPF0R); break;
    STORAGE(RPF1R); break;
    STORAGE(RPF2R); break;
    STORAGE(RPF3R); break;
    STORAGE(RPF4R); break;
    STORAGE(RPF5R); break;
    STORAGE(RPF8R); break;
    STORAGE(RPF12R); break;
    STORAGE(RPF13R); break;
    STORAGE(RPG0R); break;
    STORAGE(RPG1R); break;
    STORAGE(RPG6R); break;
    STORAGE(RPG7R); break;
    STORAGE(RPG8R); break;
    STORAGE(RPG9R); break;

    /*-------------------------------------------------------------------------
     * Real-Time Clock and Calendar.
     */
    STORAGE(RTCCON); break;
    STORAGE(RTCTIME); break;
    STORAGE(RTCDATE); break;

    /*-------------------------------------------------------------------------
     * General purpose IO signals.
     */
    STORAGE(ANSELA); break;     // Port A: analog select
    STORAGE(TRISA); break;      // Port A: mask of inputs
    STORAGE(PORTA); break;      // Port A: read inputs
    STORAGE(LATA); break;       // Port A: read outputs
    STORAGE(ODCA); break;       // Port A: open drain configuration
    STORAGE(CNPUA); break;      // Input pin pull-up
    STORAGE(CNPDA); break;      // Input pin pull-down
    STORAGE(CNCONA); break;     // Interrupt-on-change control
    STORAGE(CNENA); break;      // Input change interrupt enable
    STORAGE(CNSTATA); break;    // Input change status

    STORAGE(ANSELB); break;     // Port B: analog select
    STORAGE(TRISB); break;      // Port B: mask of inputs
    STORAGE(PORTB); break;      // Port B: read inputs
    STORAGE(LATB); break;       // Port B: read outputs
    STORAGE(ODCB); break;       // Port B: open drain configuration
    STORAGE(CNPUB); break;      // Input pin pull-up
    STORAGE(CNPDB); break;      // Input pin pull-down
    STORAGE(CNCONB); break;     // Interrupt-on-change control
    STORAGE(CNENB); break;      // Input change interrupt enable
    STORAGE(CNSTATB); break;    // Input change status

    STORAGE(ANSELC); break;     // Port C: analog select
    STORAGE(TRISC); break;      // Port C: mask of inputs
    STORAGE(PORTC); break;      // Port C: read inputs
    STORAGE(LATC); break;       // Port C: read outputs
    STORAGE(ODCC); break;       // Port C: open drain configuration
    STORAGE(CNPUC); break;      // Input pin pull-up
    STORAGE(CNPDC); break;      // Input pin pull-down
    STORAGE(CNCONC); break;     // Interrupt-on-change control
    STORAGE(CNENC); break;      // Input change interrupt enable
    STORAGE(CNSTATC); break;    // Input change status

    STORAGE(ANSELD); break;     // Port D: analog select
    STORAGE(TRISD); break;      // Port D: mask of inputs
    STORAGE(PORTD); break;      // Port D: read inputs
    STORAGE(LATD); break;       // Port D: read outputs
    STORAGE(ODCD); break;       // Port D: open drain configuration
    STORAGE(CNPUD); break;      // Input pin pull-up
    STORAGE(CNPDD); break;      // Input pin pull-down
    STORAGE(CNCOND); break;     // Interrupt-on-change control
    STORAGE(CNEND); break;      // Input change interrupt enable
    STORAGE(CNSTATD); break;    // Input change status

    STORAGE(ANSELE); break;     // Port E: analog select
    STORAGE(TRISE); break;      // Port E: mask of inputs
    STORAGE(PORTE); break;      // Port E: read inputs
    STORAGE(LATE); break;       // Port E: read outputs
    STORAGE(ODCE); break;       // Port E: open drain configuration
    STORAGE(CNPUE); break;      // Input pin pull-up
    STORAGE(CNPDE); break;      // Input pin pull-down
    STORAGE(CNCONE); break;     // Interrupt-on-change control
    STORAGE(CNENE); break;      // Input change interrupt enable
    STORAGE(CNSTATE); break;    // Input change status

    STORAGE(ANSELF); break;     // Port F: analog select
    STORAGE(TRISF); break;      // Port F: mask of inputs
    STORAGE(PORTF); break;      // Port F: read inputs
    STORAGE(LATF); break;       // Port F: read outputs
    STORAGE(ODCF); break;       // Port F: open drain configuration
    STORAGE(CNPUF); break;      // Input pin pull-up
    STORAGE(CNPDF); break;      // Input pin pull-down
    STORAGE(CNCONF); break;     // Interrupt-on-change control
    STORAGE(CNENF); break;      // Input change interrupt enable
    STORAGE(CNSTATF); break;    // Input change status

    STORAGE(ANSELG); break;     // Port G: analog select
    STORAGE(TRISG); break;      // Port G: mask of inputs
    STORAGE(PORTG); break;      // Port G: read inputs
    STORAGE(LATG); break;       // Port G: read outputs
    STORAGE(ODCG); break;       // Port G: open drain configuration
    STORAGE(CNPUG); break;      // Input pin pull-up
    STORAGE(CNPDG); break;      // Input pin pull-down
    STORAGE(CNCONG); break;     // Interrupt-on-change control
    STORAGE(CNENG); break;      // Input change interrupt enable
    STORAGE(CNSTATG); break;    // Input change status

    STORAGE(ANSELH); break;     // Port H: analog select
    STORAGE(TRISH); break;      // Port H: mask of inputs
    STORAGE(PORTH); break;      // Port H: read inputs
    STORAGE(LATH); break;       // Port H: read outputs
    STORAGE(ODCH); break;       // Port H: open drain configuration
    STORAGE(CNPUH); break;      // Input pin pull-up
    STORAGE(CNPDH); break;      // Input pin pull-down
    STORAGE(CNCONH); break;     // Interrupt-on-change control
    STORAGE(CNENH); break;      // Input change interrupt enable
    STORAGE(CNSTATH); break;    // Input change status

    STORAGE(ANSELJ); break;     // Port J: analog select
    STORAGE(TRISJ); break;      // Port J: mask of inputs
    STORAGE(PORTJ); break;      // Port J: read inputs
    STORAGE(LATJ); break;       // Port J: read outputs
    STORAGE(ODCJ); break;       // Port J: open drain configuration
    STORAGE(CNPUJ); break;      // Input pin pull-up
    STORAGE(CNPDJ); break;      // Input pin pull-down
    STORAGE(CNCONJ); break;     // Interrupt-on-change control
    STORAGE(CNENJ); break;      // Input change interrupt enable
    STORAGE(CNSTATJ); break;    // Input change status

    STORAGE(TRISK); break;      // Port K: mask of inputs
    STORAGE(PORTK); break;      // Port K: read inputs
    STORAGE(LATK); break;       // Port K: read outputs
    STORAGE(ODCK); break;       // Port K: open drain configuration
    STORAGE(CNPUK); break;      // Input pin pull-up
    STORAGE(CNPDK); break;      // Input pin pull-down
    STORAGE(CNCONK); break;     // Interrupt-on-change control
    STORAGE(CNENK); break;      // Input change interrupt enable
    STORAGE(CNSTATK); break;    // Input change status

    /*-------------------------------------------------------------------------
     * UART 1.
     */
    STORAGE(U1RXREG);                           // Receive data
        *bufp = pic32_uart_get_char(s, 0);
        break;
    STORAGE(U1BRG); break;                      // Baud rate
    STORAGE(U1MODE); break;                     // Mode
    STORAGE(U1STA);                             // Status and control
        pic32_uart_poll_status(s, 0);
        break;
    STORAGE(U1TXREG);   *bufp = 0; break;       // Transmit
    STORAGE(U1MODECLR); *bufp = 0; break;
    STORAGE(U1MODESET); *bufp = 0; break;
    STORAGE(U1MODEINV); *bufp = 0; break;
    STORAGE(U1STACLR);  *bufp = 0; break;
    STORAGE(U1STASET);  *bufp = 0; break;
    STORAGE(U1STAINV);  *bufp = 0; break;
    STORAGE(U1BRGCLR);  *bufp = 0; break;
    STORAGE(U1BRGSET);  *bufp = 0; break;
    STORAGE(U1BRGINV);  *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * UART 2.
     */
    STORAGE(U2RXREG);                           // Receive data
        *bufp = pic32_uart_get_char(s, 1);
        break;
    STORAGE(U2BRG); break;                      // Baud rate
    STORAGE(U2MODE); break;                     // Mode
    STORAGE(U2STA);                             // Status and control
        pic32_uart_poll_status(s, 1);
        break;
    STORAGE(U2TXREG);   *bufp = 0; break;       // Transmit
    STORAGE(U2MODECLR); *bufp = 0; break;
    STORAGE(U2MODESET); *bufp = 0; break;
    STORAGE(U2MODEINV); *bufp = 0; break;
    STORAGE(U2STACLR);  *bufp = 0; break;
    STORAGE(U2STASET);  *bufp = 0; break;
    STORAGE(U2STAINV);  *bufp = 0; break;
    STORAGE(U2BRGCLR);  *bufp = 0; break;
    STORAGE(U2BRGSET);  *bufp = 0; break;
    STORAGE(U2BRGINV);  *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * UART 3.
     */
    STORAGE(U3RXREG);                           // Receive data
        *bufp = pic32_uart_get_char(s, 2);
        break;
    STORAGE(U3BRG); break;                      // Baud rate
    STORAGE(U3MODE); break;                     // Mode
    STORAGE(U3STA);                             // Status and control
        pic32_uart_poll_status(s, 2);
        break;
    STORAGE(U3TXREG);   *bufp = 0; break;       // Transmit
    STORAGE(U3MODECLR); *bufp = 0; break;
    STORAGE(U3MODESET); *bufp = 0; break;
    STORAGE(U3MODEINV); *bufp = 0; break;
    STORAGE(U3STACLR);  *bufp = 0; break;
    STORAGE(U3STASET);  *bufp = 0; break;
    STORAGE(U3STAINV);  *bufp = 0; break;
    STORAGE(U3BRGCLR);  *bufp = 0; break;
    STORAGE(U3BRGSET);  *bufp = 0; break;
    STORAGE(U3BRGINV);  *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * UART 4.
     */
    STORAGE(U4RXREG);                           // Receive data
        *bufp = pic32_uart_get_char(s, 3);
        break;
    STORAGE(U4BRG); break;                      // Baud rate
    STORAGE(U4MODE); break;                     // Mode
    STORAGE(U4STA);                             // Status and control
        pic32_uart_poll_status(s, 3);
        break;
    STORAGE(U4TXREG);   *bufp = 0; break;       // Transmit
    STORAGE(U4MODECLR); *bufp = 0; break;
    STORAGE(U4MODESET); *bufp = 0; break;
    STORAGE(U4MODEINV); *bufp = 0; break;
    STORAGE(U4STACLR);  *bufp = 0; break;
    STORAGE(U4STASET);  *bufp = 0; break;
    STORAGE(U4STAINV);  *bufp = 0; break;
    STORAGE(U4BRGCLR);  *bufp = 0; break;
    STORAGE(U4BRGSET);  *bufp = 0; break;
    STORAGE(U4BRGINV);  *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * UART 5.
     */
    STORAGE(U5RXREG);                           // Receive data
        *bufp = pic32_uart_get_char(s, 4);
        break;
    STORAGE(U5BRG); break;                      // Baud rate
    STORAGE(U5MODE); break;                     // Mode
    STORAGE(U5STA);                             // Status and control
        pic32_uart_poll_status(s, 4);
        break;
    STORAGE(U5TXREG);   *bufp = 0; break;       // Transmit
    STORAGE(U5MODECLR); *bufp = 0; break;
    STORAGE(U5MODESET); *bufp = 0; break;
    STORAGE(U5MODEINV); *bufp = 0; break;
    STORAGE(U5STACLR);  *bufp = 0; break;
    STORAGE(U5STASET);  *bufp = 0; break;
    STORAGE(U5STAINV);  *bufp = 0; break;
    STORAGE(U5BRGCLR);  *bufp = 0; break;
    STORAGE(U5BRGSET);  *bufp = 0; break;
    STORAGE(U5BRGINV);  *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * UART 6.
     */
    STORAGE(U6RXREG);                           // Receive data
        *bufp = pic32_uart_get_char(s, 5);
        break;
    STORAGE(U6BRG); break;                      // Baud rate
    STORAGE(U6MODE); break;                     // Mode
    STORAGE(U6STA);                             // Status and control
        pic32_uart_poll_status(s, 5);
        break;
    STORAGE(U6TXREG);   *bufp = 0; break;       // Transmit
    STORAGE(U6MODECLR); *bufp = 0; break;
    STORAGE(U6MODESET); *bufp = 0; break;
    STORAGE(U6MODEINV); *bufp = 0; break;
    STORAGE(U6STACLR);  *bufp = 0; break;
    STORAGE(U6STASET);  *bufp = 0; break;
    STORAGE(U6STAINV);  *bufp = 0; break;
    STORAGE(U6BRGCLR);  *bufp = 0; break;
    STORAGE(U6BRGSET);  *bufp = 0; break;
    STORAGE(U6BRGINV);  *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * SPI 1.
     */
    STORAGE(SPI1CON); break;                    // Control
    STORAGE(SPI1CONCLR); *bufp = 0; break;
    STORAGE(SPI1CONSET); *bufp = 0; break;
    STORAGE(SPI1CONINV); *bufp = 0; break;
    STORAGE(SPI1STAT); break;                   // Status
    STORAGE(SPI1STATCLR); *bufp = 0; break;
    STORAGE(SPI1STATSET); *bufp = 0; break;
    STORAGE(SPI1STATINV); *bufp = 0; break;
    STORAGE(SPI1BUF);                           // Buffer
        *bufp = pic32_spi_readbuf(s, 0);
        break;
    STORAGE(SPI1BRG); break;                    // Baud rate
    STORAGE(SPI1BRGCLR); *bufp = 0; break;
    STORAGE(SPI1BRGSET); *bufp = 0; break;
    STORAGE(SPI1BRGINV); *bufp = 0; break;
    STORAGE(SPI1CON2); break;                   // Control 2
    STORAGE(SPI1CON2CLR); *bufp = 0; break;
    STORAGE(SPI1CON2SET); *bufp = 0; break;
    STORAGE(SPI1CON2INV); *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * SPI 2.
     */
    STORAGE(SPI2CON); break;                    // Control
    STORAGE(SPI2CONCLR); *bufp = 0; break;
    STORAGE(SPI2CONSET); *bufp = 0; break;
    STORAGE(SPI2CONINV); *bufp = 0; break;
    STORAGE(SPI2STAT); break;                   // Status
    STORAGE(SPI2STATCLR); *bufp = 0; break;
    STORAGE(SPI2STATSET); *bufp = 0; break;
    STORAGE(SPI2STATINV); *bufp = 0; break;
    STORAGE(SPI2BUF);                           // Buffer
        *bufp = pic32_spi_readbuf(s, 1);
        break;
    STORAGE(SPI2BRG); break;                    // Baud rate
    STORAGE(SPI2BRGCLR); *bufp = 0; break;
    STORAGE(SPI2BRGSET); *bufp = 0; break;
    STORAGE(SPI2BRGINV); *bufp = 0; break;
    STORAGE(SPI2CON2); break;                   // Control 2
    STORAGE(SPI2CON2CLR); *bufp = 0; break;
    STORAGE(SPI2CON2SET); *bufp = 0; break;
    STORAGE(SPI2CON2INV); *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * SPI 3.
     */
    STORAGE(SPI3CON); break;                    // Control
    STORAGE(SPI3CONCLR); *bufp = 0; break;
    STORAGE(SPI3CONSET); *bufp = 0; break;
    STORAGE(SPI3CONINV); *bufp = 0; break;
    STORAGE(SPI3STAT); break;                   // Status
    STORAGE(SPI3STATCLR); *bufp = 0; break;
    STORAGE(SPI3STATSET); *bufp = 0; break;
    STORAGE(SPI3STATINV); *bufp = 0; break;
    STORAGE(SPI3BUF);                           // SPIx Buffer
        *bufp = pic32_spi_readbuf(s, 2);
        break;
    STORAGE(SPI3BRG); break;                    // Baud rate
    STORAGE(SPI3BRGCLR); *bufp = 0; break;
    STORAGE(SPI3BRGSET); *bufp = 0; break;
    STORAGE(SPI3BRGINV); *bufp = 0; break;
    STORAGE(SPI3CON2); break;                   // Control 2
    STORAGE(SPI3CON2CLR); *bufp = 0; break;
    STORAGE(SPI3CON2SET); *bufp = 0; break;
    STORAGE(SPI3CON2INV); *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * SPI 4.
     */
    STORAGE(SPI4CON); break;                    // Control
    STORAGE(SPI4CONCLR); *bufp = 0; break;
    STORAGE(SPI4CONSET); *bufp = 0; break;
    STORAGE(SPI4CONINV); *bufp = 0; break;
    STORAGE(SPI4STAT); break;                   // Status
    STORAGE(SPI4STATCLR); *bufp = 0; break;
    STORAGE(SPI4STATSET); *bufp = 0; break;
    STORAGE(SPI4STATINV); *bufp = 0; break;
    STORAGE(SPI4BUF);                           // Buffer
        *bufp = pic32_spi_readbuf(s, 3);
        break;
    STORAGE(SPI4BRG); break;                    // Baud rate
    STORAGE(SPI4BRGCLR); *bufp = 0; break;
    STORAGE(SPI4BRGSET); *bufp = 0; break;
    STORAGE(SPI4BRGINV); *bufp = 0; break;
    STORAGE(SPI4CON2); break;                   // Control 2
    STORAGE(SPI4CON2CLR); *bufp = 0; break;
    STORAGE(SPI4CON2SET); *bufp = 0; break;
    STORAGE(SPI4CON2INV); *bufp = 0; break;

    /*-------------------------------------------------------------------------
     * Timers.
     */
    STORAGE(T1CON); break;
    STORAGE(TMR1); break;
    STORAGE(PR1); break;
    STORAGE(T2CON); break;
    STORAGE(TMR2); break;
    STORAGE(PR2); break;
    STORAGE(T3CON); break;
    STORAGE(TMR3); break;
    STORAGE(PR3); break;
    STORAGE(T4CON); break;
    STORAGE(TMR4); break;
    STORAGE(PR4); break;
    STORAGE(T5CON); break;
    STORAGE(TMR5); break;
    STORAGE(PR5); break;
    STORAGE(T6CON); break;
    STORAGE(TMR6); break;
    STORAGE(PR6); break;
    STORAGE(T7CON); break;
    STORAGE(TMR7); break;
    STORAGE(PR7); break;
    STORAGE(T8CON); break;
    STORAGE(TMR8); break;
    STORAGE(PR8); break;
    STORAGE(T9CON); break;
    STORAGE(TMR9); break;
    STORAGE(PR9); break;

    /*-------------------------------------------------------------------------
     * Ethernet.
     */
    STORAGE(ETHCON1); break;            // Control 1
    STORAGE(ETHCON2); break;            // Control 2: RX data buffer size
    STORAGE(ETHTXST); break;            // Tx descriptor start address
    STORAGE(ETHRXST); break;            // Rx descriptor start address
    STORAGE(ETHHT0); break;             // Hash tasble 0
    STORAGE(ETHHT1); break;             // Hash tasble 1
    STORAGE(ETHPMM0); break;            // Pattern match mask 0
    STORAGE(ETHPMM1); break;            // Pattern match mask 1
    STORAGE(ETHPMCS); break;            // Pattern match checksum
    STORAGE(ETHPMO); break;             // Pattern match offset
    STORAGE(ETHRXFC); break;            // Receive filter configuration
    STORAGE(ETHRXWM); break;            // Receive watermarks
    STORAGE(ETHIEN); break;             // Interrupt enable
    STORAGE(ETHIRQ); break;             // Interrupt request
    STORAGE(ETHSTAT); break;            // Status
    STORAGE(ETHRXOVFLOW); break;        // Receive overflow statistics
    STORAGE(ETHFRMTXOK); break;         // Frames transmitted OK statistics
    STORAGE(ETHSCOLFRM); break;         // Single collision frames statistics
    STORAGE(ETHMCOLFRM); break;         // Multiple collision frames statistics
    STORAGE(ETHFRMRXOK); break;         // Frames received OK statistics
    STORAGE(ETHFCSERR); break;          // Frame check sequence error statistics
    STORAGE(ETHALGNERR); break;         // Alignment errors statistics
    STORAGE(EMAC1CFG1); break;          // MAC configuration 1
    STORAGE(EMAC1CFG2); break;          // MAC configuration 2
    STORAGE(EMAC1IPGT); break;          // MAC back-to-back interpacket gap
    STORAGE(EMAC1IPGR); break;          // MAC non-back-to-back interpacket gap
    STORAGE(EMAC1CLRT); break;          // MAC collision window/retry limit
    STORAGE(EMAC1MAXF); break;          // MAC maximum frame length
    STORAGE(EMAC1SUPP); break;          // MAC PHY support
    STORAGE(EMAC1TEST); break;          // MAC test
    STORAGE(EMAC1MCFG); break;          // MII configuration
    STORAGE(EMAC1MCMD); break;          // MII command
    STORAGE(EMAC1MADR); break;          // MII address
    STORAGE(EMAC1MWTD); break;          // MII write data
    STORAGE(EMAC1MRDD); break;          // MII read data
    STORAGE(EMAC1MIND); break;          // MII indicators
    STORAGE(EMAC1SA0); break;           // MAC station address 0
    STORAGE(EMAC1SA1); break;           // MAC station address 1
    STORAGE(EMAC1SA2); break;           // MAC station address 2

    /*-------------------------------------------------------------------------
     * USB.
     */
    STORAGE(USBCSR0); break;
    STORAGE(USBCSR1); break;
    STORAGE(USBCSR2); break;
    STORAGE(USBCSR3); break;
    STORAGE(USBIENCSR0); break;
    STORAGE(USBIENCSR1); break;
    STORAGE(USBIENCSR2); break;
    STORAGE(USBIENCSR3); break;
    STORAGE(USBFIFO0); break;
    STORAGE(USBFIFO1); break;
    STORAGE(USBFIFO2); break;
    STORAGE(USBFIFO3); break;
    STORAGE(USBFIFO4); break;
    STORAGE(USBFIFO5); break;
    STORAGE(USBFIFO6); break;
    STORAGE(USBFIFO7); break;
    STORAGE(USBOTG); break;
    STORAGE(USBFIFOA); break;
    STORAGE(USBHWVER); break;
    STORAGE(USBINFO); break;
    STORAGE(USBEOFRST); break;
    STORAGE(USBE0TXA); break;
    STORAGE(USBE0RXA); break;
    STORAGE(USBE1TXA); break;
    STORAGE(USBE1RXA); break;
    STORAGE(USBE2TXA); break;
    STORAGE(USBE2RXA); break;
    STORAGE(USBE3TXA); break;
    STORAGE(USBE3RXA); break;
    STORAGE(USBE4TXA); break;
    STORAGE(USBE4RXA); break;
    STORAGE(USBE5TXA); break;
    STORAGE(USBE5RXA); break;
    STORAGE(USBE6TXA); break;
    STORAGE(USBE6RXA); break;
    STORAGE(USBE7TXA); break;
    STORAGE(USBE7RXA); break;
    STORAGE(USBE0CSR0); break;
    STORAGE(USBE0CSR2); break;
    STORAGE(USBE0CSR3); break;
    STORAGE(USBE1CSR0); break;
    STORAGE(USBE1CSR1); break;
    STORAGE(USBE1CSR2); break;
    STORAGE(USBE1CSR3); break;
    STORAGE(USBE2CSR0); break;
    STORAGE(USBE2CSR1); break;
    STORAGE(USBE2CSR2); break;
    STORAGE(USBE2CSR3); break;
    STORAGE(USBE3CSR0); break;
    STORAGE(USBE3CSR1); break;
    STORAGE(USBE3CSR2); break;
    STORAGE(USBE3CSR3); break;
    STORAGE(USBE4CSR0); break;
    STORAGE(USBE4CSR1); break;
    STORAGE(USBE4CSR2); break;
    STORAGE(USBE4CSR3); break;
    STORAGE(USBE5CSR0); break;
    STORAGE(USBE5CSR1); break;
    STORAGE(USBE5CSR2); break;
    STORAGE(USBE5CSR3); break;
    STORAGE(USBE6CSR0); break;
    STORAGE(USBE6CSR1); break;
    STORAGE(USBE6CSR2); break;
    STORAGE(USBE6CSR3); break;
    STORAGE(USBE7CSR0); break;
    STORAGE(USBE7CSR1); break;
    STORAGE(USBE7CSR2); break;
    STORAGE(USBE7CSR3); break;
    STORAGE(USBDMAINT); break;
    STORAGE(USBDMA1C); break;
    STORAGE(USBDMA1A); break;
    STORAGE(USBDMA1N); break;
    STORAGE(USBDMA2C); break;
    STORAGE(USBDMA2A); break;
    STORAGE(USBDMA2N); break;
    STORAGE(USBDMA3C); break;
    STORAGE(USBDMA3A); break;
    STORAGE(USBDMA3N); break;
    STORAGE(USBDMA4C); break;
    STORAGE(USBDMA4A); break;
    STORAGE(USBDMA4N); break;
    STORAGE(USBDMA5C); break;
    STORAGE(USBDMA5A); break;
    STORAGE(USBDMA5N); break;
    STORAGE(USBDMA6C); break;
    STORAGE(USBDMA6A); break;
    STORAGE(USBDMA6N); break;
    STORAGE(USBDMA7C); break;
    STORAGE(USBDMA7A); break;
    STORAGE(USBDMA7N); break;
    STORAGE(USBDMA8C); break;
    STORAGE(USBDMA8A); break;
    STORAGE(USBDMA8N); break;
    STORAGE(USBE1RPC); break;
    STORAGE(USBE2RPC); break;
    STORAGE(USBE3RPC); break;
    STORAGE(USBE4RPC); break;
    STORAGE(USBE5RPC); break;
    STORAGE(USBE6RPC); break;
    STORAGE(USBE7RPC); break;
    STORAGE(USBDPBFD); break;
    STORAGE(USBTMCON1); break;
    STORAGE(USBTMCON2); break;
    STORAGE(USBLPMR1); break;
    STORAGE(USBLMPR2); break;

    default:
        printf("--- Read 1f8%05x: peripheral register not supported\n",
            offset);
        if (qemu_loglevel_mask(CPU_LOG_INSTR))
            fprintf(qemu_logfile, "--- Read 1f8%05x: peripheral register not supported\n",
                offset);
        exit(1);
    }
    return *bufp;
}

static void pps_input_group1(unsigned address, unsigned data)
{
    // 0000 = RPD1
    // 0001 = RPG9
    // 0010 = RPB14
    // 0011 = RPD0
    // 0101 = RPB6
    // 0110 = RPD5
    // 0111 = RPB2
    // 1000 = RPF3
    // 1001 = RPF13
    // 1011 = RPF2
    // 1100 = RPC2
    // 1101 = RPE8
}

static void pps_input_group2(unsigned address, unsigned data)
{
    // 0000 = RPD9
    // 0001 = RPG6
    // 0010 = RPB8
    // 0011 = RPB15
    // 0100 = RPD4
    // 0101 = RPB0
    // 0110 = RPE3
    // 0111 = RPB7
    // 1001 = RPF12
    // 1010 = RPD12
    // 1011 = RPF8
    // 1100 = RPC3
    // 1101 = RPE9
}

static void pps_input_group3(unsigned address, unsigned data)
{
    // 0000 = RPD2
    // 0001 = RPG8
    // 0010 = RPF4
    // 0011 = RPD10
    // 0100 = RPF1
    // 0101 = RPB9
    // 0110 = RPB10
    // 0111 = RPC14
    // 1000 = RPB5
    // 1010 = RPC1
    // 1011 = RPD14
    // 1100 = RPG1
    // 1101 = RPA14
    // 1110 = RPD6
}

static void pps_input_group4(unsigned address, unsigned data)
{
    // 0000 = RPD3
    // 0001 = RPG7
    // 0010 = RPF5
    // 0011 = RPD11
    // 0100 = RPF0
    // 0101 = RPB1
    // 0110 = RPE5
    // 0111 = RPC13
    // 1000 = RPB3
    // 1010 = RPC4
    // 1011 = RPD15
    // 1100 = RPG0
    // 1101 = RPA15
    // 1110 = RPD7
}

static void pps_output_group1(unsigned address, unsigned data)
{
    // 0000 = No Connect
    // 0001 = U1TX
    // 0010 = U2RTS
    // 0011 = U5TX
    // 0100 = U6RTS
    // 0101 = SDO1
    // 0110 = SDO2
    // 0111 = SDO3
    // 1000 = SDO4
    // 1001 = SDO5
    // 1011 = OC4
    // 1100 = OC7
    // 1111 = REFCLKO1
}

static void pps_output_group2(unsigned address, unsigned data)
{
    // 0000 = No Connect
    // 0001 = U1RTS
    // 0010 = U2TX
    // 0011 = U5RTS
    // 0100 = U6TX
    // 0110 = SS2
    // 1000 = SDO4
    // 1010 = SDO6
    // 1011 = OC2
    // 1100 = OC1
    // 1101 = OC9
    // 1111 = C2TX
}

static void pps_output_group3(unsigned address, unsigned data)
{
    // 0000 = No Connect
    // 0001 = U3TX
    // 0010 = U4RTS
    // 0101 = SDO1
    // 0110 = SDO2
    // 0111 = SDO3
    // 1001 = SDO5
    // 1010 = SS6
    // 1011 = OC3
    // 1100 = OC6
    // 1101 = REFCLKO4
    // 1110 = C2OUT
    // 1111 = C1TX
}

static void pps_output_group4(unsigned address, unsigned data)
{
    // 0000 = No Connect
    // 0001 = U3RTS
    // 0010 = U4TX
    // 0100 = U6TX
    // 0101 = SS1
    // 0111 = SS3
    // 1000 = SS4
    // 1001 = SS5
    // 1010 = SDO6
    // 1011 = OC5
    // 1100 = OC8
    // 1110 = C1OUT
    // 1111 = REFCLKO3
}

static void io_write32(pic32_t *s, unsigned offset, unsigned data, const char **namep)
{
    unsigned *bufp = &VALUE(offset);
    unsigned mask;

    switch (offset) {
    /*-------------------------------------------------------------------------
     * Interrupt controller registers.
     */
    WRITEOP(INTCON); return;    // Interrupt Control
    READONLY(INTSTAT);          // Interrupt Status
    WRITEOP(IPTMR);  return;    // Temporal Proximity Timer
    WRITEOP(IFS0); goto irq;    // IFS(0..2) - Interrupt Flag Status
    WRITEOP(IFS1); goto irq;
    WRITEOP(IFS2); goto irq;
    WRITEOP(IFS3); goto irq;
    WRITEOP(IFS4); goto irq;
    WRITEOP(IFS5); goto irq;
    WRITEOP(IEC0); goto irq;    // IEC(0..2) - Interrupt Enable Control
    WRITEOP(IEC1); goto irq;
    WRITEOP(IEC2); goto irq;
    WRITEOP(IEC3); goto irq;
    WRITEOP(IEC4); goto irq;
    WRITEOP(IEC5); goto irq;

    // IPC(0..11) - Interrupt Priority Control
    WRITEOP(IPC0); goto irq;    WRITEOP(IPC1); goto irq;
    WRITEOP(IPC2); goto irq;    WRITEOP(IPC3); goto irq;
    WRITEOP(IPC4); goto irq;    WRITEOP(IPC5); goto irq;
    WRITEOP(IPC6); goto irq;    WRITEOP(IPC7); goto irq;
    WRITEOP(IPC8); goto irq;    WRITEOP(IPC9); goto irq;
    WRITEOP(IPC10); goto irq;   WRITEOP(IPC11); goto irq;
    WRITEOP(IPC12); goto irq;   WRITEOP(IPC13); goto irq;
    WRITEOP(IPC14); goto irq;   WRITEOP(IPC15); goto irq;
    WRITEOP(IPC16); goto irq;   WRITEOP(IPC17); goto irq;
    WRITEOP(IPC18); goto irq;   WRITEOP(IPC19); goto irq;
    WRITEOP(IPC20); goto irq;   WRITEOP(IPC21); goto irq;
    WRITEOP(IPC22); goto irq;   WRITEOP(IPC23); goto irq;
    WRITEOP(IPC24); goto irq;   WRITEOP(IPC25); goto irq;
    WRITEOP(IPC26); goto irq;   WRITEOP(IPC27); goto irq;
    WRITEOP(IPC28); goto irq;   WRITEOP(IPC29); goto irq;
    WRITEOP(IPC30); goto irq;   WRITEOP(IPC31); goto irq;
    WRITEOP(IPC32); goto irq;   WRITEOP(IPC33); goto irq;
    WRITEOP(IPC34); goto irq;   WRITEOP(IPC35); goto irq;
    WRITEOP(IPC36); goto irq;   WRITEOP(IPC37); goto irq;
    WRITEOP(IPC38); goto irq;   WRITEOP(IPC39); goto irq;
    WRITEOP(IPC40); goto irq;   WRITEOP(IPC41); goto irq;
    WRITEOP(IPC42); goto irq;   WRITEOP(IPC43); goto irq;
    WRITEOP(IPC44); goto irq;   WRITEOP(IPC45); goto irq;
    WRITEOP(IPC46); goto irq;   WRITEOP(IPC47);
irq:    update_irq_status(s);
        return;

    // OFF000..OFF190 - Interrupt Vector Address Offset
    STORAGE(OFF(0)); break;     STORAGE(OFF(1)); break;
    STORAGE(OFF(2)); break;     STORAGE(OFF(3)); break;
    STORAGE(OFF(4)); break;     STORAGE(OFF(5)); break;
    STORAGE(OFF(6)); break;     STORAGE(OFF(7)); break;
    STORAGE(OFF(8)); break;     STORAGE(OFF(9)); break;
    STORAGE(OFF(10)); break;    STORAGE(OFF(11)); break;
    STORAGE(OFF(12)); break;    STORAGE(OFF(13)); break;
    STORAGE(OFF(14)); break;    STORAGE(OFF(15)); break;
    STORAGE(OFF(16)); break;    STORAGE(OFF(17)); break;
    STORAGE(OFF(18)); break;    STORAGE(OFF(19)); break;
    STORAGE(OFF(20)); break;    STORAGE(OFF(21)); break;
    STORAGE(OFF(22)); break;    STORAGE(OFF(23)); break;
    STORAGE(OFF(24)); break;    STORAGE(OFF(25)); break;
    STORAGE(OFF(26)); break;    STORAGE(OFF(27)); break;
    STORAGE(OFF(28)); break;    STORAGE(OFF(29)); break;
    STORAGE(OFF(30)); break;    STORAGE(OFF(31)); break;
    STORAGE(OFF(32)); break;    STORAGE(OFF(33)); break;
    STORAGE(OFF(34)); break;    STORAGE(OFF(35)); break;
    STORAGE(OFF(36)); break;    STORAGE(OFF(37)); break;
    STORAGE(OFF(38)); break;    STORAGE(OFF(39)); break;
    STORAGE(OFF(40)); break;    STORAGE(OFF(41)); break;
    STORAGE(OFF(42)); break;    STORAGE(OFF(43)); break;
    STORAGE(OFF(44)); break;    STORAGE(OFF(45)); break;
    STORAGE(OFF(46)); break;    STORAGE(OFF(47)); break;
    STORAGE(OFF(48)); break;    STORAGE(OFF(49)); break;
    STORAGE(OFF(50)); break;    STORAGE(OFF(51)); break;
    STORAGE(OFF(52)); break;    STORAGE(OFF(53)); break;
    STORAGE(OFF(54)); break;    STORAGE(OFF(55)); break;
    STORAGE(OFF(56)); break;    STORAGE(OFF(57)); break;
    STORAGE(OFF(58)); break;    STORAGE(OFF(59)); break;
    STORAGE(OFF(60)); break;    STORAGE(OFF(61)); break;
    STORAGE(OFF(62)); break;    STORAGE(OFF(63)); break;
    STORAGE(OFF(64)); break;    STORAGE(OFF(65)); break;
    STORAGE(OFF(66)); break;    STORAGE(OFF(67)); break;
    STORAGE(OFF(68)); break;    STORAGE(OFF(69)); break;
    STORAGE(OFF(70)); break;    STORAGE(OFF(71)); break;
    STORAGE(OFF(72)); break;    STORAGE(OFF(73)); break;
    STORAGE(OFF(74)); break;    STORAGE(OFF(75)); break;
    STORAGE(OFF(76)); break;    STORAGE(OFF(77)); break;
    STORAGE(OFF(78)); break;    STORAGE(OFF(79)); break;
    STORAGE(OFF(80)); break;    STORAGE(OFF(81)); break;
    STORAGE(OFF(82)); break;    STORAGE(OFF(83)); break;
    STORAGE(OFF(84)); break;    STORAGE(OFF(85)); break;
    STORAGE(OFF(86)); break;    STORAGE(OFF(87)); break;
    STORAGE(OFF(88)); break;    STORAGE(OFF(89)); break;
    STORAGE(OFF(90)); break;    STORAGE(OFF(91)); break;
    STORAGE(OFF(92)); break;    STORAGE(OFF(93)); break;
    STORAGE(OFF(94)); break;    STORAGE(OFF(95)); break;
    STORAGE(OFF(96)); break;    STORAGE(OFF(97)); break;
    STORAGE(OFF(98)); break;    STORAGE(OFF(99)); break;
    STORAGE(OFF(100)); break;   STORAGE(OFF(101)); break;
    STORAGE(OFF(102)); break;   STORAGE(OFF(103)); break;
    STORAGE(OFF(104)); break;   STORAGE(OFF(105)); break;
    STORAGE(OFF(106)); break;   STORAGE(OFF(107)); break;
    STORAGE(OFF(108)); break;   STORAGE(OFF(109)); break;
    STORAGE(OFF(110)); break;   STORAGE(OFF(111)); break;
    STORAGE(OFF(112)); break;   STORAGE(OFF(113)); break;
    STORAGE(OFF(114)); break;   STORAGE(OFF(115)); break;
    STORAGE(OFF(116)); break;   STORAGE(OFF(117)); break;
    STORAGE(OFF(118)); break;   STORAGE(OFF(119)); break;
    STORAGE(OFF(120)); break;   STORAGE(OFF(121)); break;
    STORAGE(OFF(122)); break;   STORAGE(OFF(123)); break;
    STORAGE(OFF(124)); break;   STORAGE(OFF(125)); break;
    STORAGE(OFF(126)); break;   STORAGE(OFF(127)); break;
    STORAGE(OFF(128)); break;   STORAGE(OFF(129)); break;
    STORAGE(OFF(130)); break;   STORAGE(OFF(131)); break;
    STORAGE(OFF(132)); break;   STORAGE(OFF(133)); break;
    STORAGE(OFF(134)); break;   STORAGE(OFF(135)); break;
    STORAGE(OFF(136)); break;   STORAGE(OFF(137)); break;
    STORAGE(OFF(138)); break;   STORAGE(OFF(139)); break;
    STORAGE(OFF(140)); break;   STORAGE(OFF(141)); break;
    STORAGE(OFF(142)); break;   STORAGE(OFF(143)); break;
    STORAGE(OFF(144)); break;   STORAGE(OFF(145)); break;
    STORAGE(OFF(146)); break;   STORAGE(OFF(147)); break;
    STORAGE(OFF(148)); break;   STORAGE(OFF(149)); break;
    STORAGE(OFF(150)); break;   STORAGE(OFF(151)); break;
    STORAGE(OFF(152)); break;   STORAGE(OFF(153)); break;
    STORAGE(OFF(154)); break;   STORAGE(OFF(155)); break;
    STORAGE(OFF(156)); break;   STORAGE(OFF(157)); break;
    STORAGE(OFF(158)); break;   STORAGE(OFF(159)); break;
    STORAGE(OFF(160)); break;   STORAGE(OFF(161)); break;
    STORAGE(OFF(162)); break;   STORAGE(OFF(163)); break;
    STORAGE(OFF(164)); break;   STORAGE(OFF(165)); break;
    STORAGE(OFF(166)); break;   STORAGE(OFF(167)); break;
    STORAGE(OFF(168)); break;   STORAGE(OFF(169)); break;
    STORAGE(OFF(170)); break;   STORAGE(OFF(171)); break;
    STORAGE(OFF(172)); break;   STORAGE(OFF(173)); break;
    STORAGE(OFF(174)); break;   STORAGE(OFF(175)); break;
    STORAGE(OFF(176)); break;   STORAGE(OFF(177)); break;
    STORAGE(OFF(178)); break;   STORAGE(OFF(179)); break;
    STORAGE(OFF(180)); break;   STORAGE(OFF(181)); break;
    STORAGE(OFF(182)); break;   STORAGE(OFF(183)); break;
    STORAGE(OFF(184)); break;   STORAGE(OFF(185)); break;
    STORAGE(OFF(186)); break;   STORAGE(OFF(187)); break;
    STORAGE(OFF(188)); break;   STORAGE(OFF(189)); break;
    STORAGE(OFF(190)); break;

    /*-------------------------------------------------------------------------
     * Prefetch controller.
     */
    WRITEOP(PRECON); return;    // Prefetch Control
    WRITEOP(PRESTAT); return;   // Prefetch Status

    /*-------------------------------------------------------------------------
     * System controller.
     */
    STORAGE(CFGCON);            // Configuration Control
        // TODO: use unlock sequence
        mask = PIC32_CFGCON_DMAPRI | PIC32_CFGCON_CPUPRI |
            PIC32_CFGCON_ICACLK | PIC32_CFGCON_OCACLK |
            PIC32_CFGCON_IOLOCK | PIC32_CFGCON_PMDLOCK |
            PIC32_CFGCON_PGLOCK | PIC32_CFGCON_USBSSEN |
            PIC32_CFGCON_ECC_MASK | PIC32_CFGCON_JTAGEN |
            PIC32_CFGCON_TROEN | PIC32_CFGCON_TDOEN;
        data = (data & mask) | (*bufp & ~mask);
        break;
    READONLY(DEVID);            // Device Identifier
    STORAGE(SYSKEY);            // System Key
        /* Unlock state machine. */
        if (s->syskey_unlock == 0 && VALUE(SYSKEY) == 0xaa996655)
            s->syskey_unlock = 1;
        if (s->syskey_unlock == 1 && VALUE(SYSKEY) == 0x556699aa)
            s->syskey_unlock = 2;
        else
            s->syskey_unlock = 0;
        break;
    STORAGE(RCON); break;       // Reset Control
    WRITEOP(RSWRST);            // Software Reset
        if (s->syskey_unlock == 2 && (VALUE(RSWRST) & 1)) {
            /* Reset CPU. */
            qemu_system_reset_request();

            /* Reset all devices */
            io_reset(s);
            pic32_sdcard_reset(s);
        }
        break;
    STORAGE(OSCCON); break;     // Oscillator Control
    STORAGE(OSCTUN); break;     // Oscillator Tuning
    STORAGE(SPLLCON); break;    // System PLL Control
    WRITEOP(REFO1CON); return;
    WRITEOP(REFO2CON); return;
    WRITEOP(REFO3CON); return;
    WRITEOP(REFO4CON); return;
    WRITEOP(PB1DIV); return;    // Peripheral bus 1 divisor
    WRITEOP(PB2DIV); return;    // Peripheral bus 2 divisor
    WRITEOP(PB3DIV); return;    // Peripheral bus 3 divisor
    WRITEOP(PB4DIV); return;    // Peripheral bus 4 divisor
    WRITEOP(PB5DIV); return;    // Peripheral bus 5 divisor
    WRITEOP(PB7DIV); return;    // Peripheral bus 7 divisor
    WRITEOP(PB8DIV); return;    // Peripheral bus 8 divisor

    /*-------------------------------------------------------------------------
     * Peripheral port select registers: input.
     */
    STORAGE(INT1R);    pps_input_group1(offset, data); break;
    STORAGE(T4CKR);    pps_input_group1(offset, data); break;
    STORAGE(T9CKR);    pps_input_group1(offset, data); break;
    STORAGE(IC1R);     pps_input_group1(offset, data); break;
    STORAGE(IC6R);     pps_input_group1(offset, data); break;
    STORAGE(U3CTSR);   pps_input_group1(offset, data); break;
    STORAGE(U4RXR);    pps_input_group1(offset, data); break;
    STORAGE(U6RXR);    pps_input_group1(offset, data); break;
    STORAGE(SS2R);     pps_input_group1(offset, data); break;
    STORAGE(SDI6R);    pps_input_group1(offset, data); break;
    STORAGE(OCFAR);    pps_input_group1(offset, data); break;
    STORAGE(REFCLKI3R);pps_input_group1(offset, data); break;

    STORAGE(INT2R);    pps_input_group2(offset, data); break;
    STORAGE(T3CKR);    pps_input_group2(offset, data); break;
    STORAGE(T8CKR);    pps_input_group2(offset, data); break;
    STORAGE(IC2R);     pps_input_group2(offset, data); break;
    STORAGE(IC5R);     pps_input_group2(offset, data); break;
    STORAGE(IC9R);     pps_input_group2(offset, data); break;
    STORAGE(U1CTSR);   pps_input_group2(offset, data); break;
    STORAGE(U2RXR);    pps_input_group2(offset, data); break;
    STORAGE(U5CTSR);   pps_input_group2(offset, data); break;
    STORAGE(SS1R);     pps_input_group2(offset, data); break;
    STORAGE(SS3R);     pps_input_group2(offset, data); break;
    STORAGE(SS4R);     pps_input_group2(offset, data); break;
    STORAGE(SS5R);     pps_input_group2(offset, data); break;
    STORAGE(C2RXR);    pps_input_group2(offset, data); break;

    STORAGE(INT3R);    pps_input_group3(offset, data); break;
    STORAGE(T2CKR);    pps_input_group3(offset, data); break;
    STORAGE(T6CKR);    pps_input_group3(offset, data); break;
    STORAGE(IC3R);     pps_input_group3(offset, data); break;
    STORAGE(IC7R);     pps_input_group3(offset, data); break;
    STORAGE(U1RXR);    pps_input_group3(offset, data); break;
    STORAGE(U2CTSR);   pps_input_group3(offset, data); break;
    STORAGE(U5RXR);    pps_input_group3(offset, data); break;
    STORAGE(U6CTSR);   pps_input_group3(offset, data); break;
    STORAGE(SDI1R);    pps_input_group3(offset, data); break;
    STORAGE(SDI3R);    pps_input_group3(offset, data); break;
    STORAGE(SDI5R);    pps_input_group3(offset, data); break;
    STORAGE(SS6R);     pps_input_group3(offset, data); break;
    STORAGE(REFCLKI1R);pps_input_group3(offset, data); break;

    STORAGE(INT4R);    pps_input_group4(offset, data); break;
    STORAGE(T5CKR);    pps_input_group4(offset, data); break;
    STORAGE(T7CKR);    pps_input_group4(offset, data); break;
    STORAGE(IC4R);     pps_input_group4(offset, data); break;
    STORAGE(IC8R);     pps_input_group4(offset, data); break;
    STORAGE(U3RXR);    pps_input_group4(offset, data); break;
    STORAGE(U4CTSR);   pps_input_group4(offset, data); break;
    STORAGE(SDI2R);    pps_input_group4(offset, data); break;
    STORAGE(SDI4R);    pps_input_group4(offset, data); break;
    STORAGE(C1RXR);    pps_input_group4(offset, data); break;
    STORAGE(REFCLKI4R);pps_input_group4(offset, data); break;

    /*-------------------------------------------------------------------------
     * Peripheral port select registers: output.
     */
    STORAGE(RPA15R);   pps_output_group1(offset, data); break;
    STORAGE(RPB1R);    pps_output_group1(offset, data); break;
    STORAGE(RPB3R);    pps_output_group1(offset, data); break;
    STORAGE(RPC4R);    pps_output_group1(offset, data); break;
    STORAGE(RPC13R);   pps_output_group1(offset, data); break;
    STORAGE(RPD3R);    pps_output_group1(offset, data); break;
    STORAGE(RPD7R);    pps_output_group1(offset, data); break;
    STORAGE(RPD11R);   pps_output_group1(offset, data); break;
    STORAGE(RPD15R);   pps_output_group1(offset, data); break;
    STORAGE(RPE5R);    pps_output_group1(offset, data); break;
    STORAGE(RPF0R);    pps_output_group1(offset, data); break;
    STORAGE(RPF5R);    pps_output_group1(offset, data); break;
    STORAGE(RPG0R);    pps_output_group1(offset, data); break;
    STORAGE(RPG7R);    pps_output_group1(offset, data); break;

    STORAGE(RPB2R);    pps_output_group2(offset, data); break;
    STORAGE(RPB6R);    pps_output_group2(offset, data); break;
    STORAGE(RPB14R);   pps_output_group2(offset, data); break;
    STORAGE(RPC2R);    pps_output_group2(offset, data); break;
    STORAGE(RPD0R);    pps_output_group2(offset, data); break;
    STORAGE(RPD1R);    pps_output_group2(offset, data); break;
    STORAGE(RPD5R);    pps_output_group2(offset, data); break;
    STORAGE(RPE8R);    pps_output_group2(offset, data); break;
    STORAGE(RPF2R);    pps_output_group2(offset, data); break;
    STORAGE(RPF3R);    pps_output_group2(offset, data); break;
    STORAGE(RPF13R);   pps_output_group2(offset, data); break;
    STORAGE(RPG9R);    pps_output_group2(offset, data); break;

    STORAGE(RPA14R);   pps_output_group3(offset, data); break;
    STORAGE(RPB5R);    pps_output_group3(offset, data); break;
    STORAGE(RPB9R);    pps_output_group3(offset, data); break;
    STORAGE(RPB10R);   pps_output_group3(offset, data); break;
    STORAGE(RPC1R);    pps_output_group3(offset, data); break;
    STORAGE(RPC14R);   pps_output_group3(offset, data); break;
    STORAGE(RPD2R);    pps_output_group3(offset, data); break;
    STORAGE(RPD6R);    pps_output_group3(offset, data); break;
    STORAGE(RPD10R);   pps_output_group3(offset, data); break;
    STORAGE(RPD14R);   pps_output_group3(offset, data); break;
    STORAGE(RPF1R);    pps_output_group3(offset, data); break;
    STORAGE(RPF4R);    pps_output_group3(offset, data); break;
    STORAGE(RPG1R);    pps_output_group3(offset, data); break;
    STORAGE(RPG8R);    pps_output_group3(offset, data); break;

    STORAGE(RPB0R);    pps_output_group4(offset, data); break;
    STORAGE(RPB7R);    pps_output_group4(offset, data); break;
    STORAGE(RPB8R);    pps_output_group4(offset, data); break;
    STORAGE(RPB15R);   pps_output_group4(offset, data); break;
    STORAGE(RPC3R);    pps_output_group4(offset, data); break;
    STORAGE(RPD4R);    pps_output_group4(offset, data); break;
    STORAGE(RPD9R);    pps_output_group4(offset, data); break;
    STORAGE(RPD12R);   pps_output_group4(offset, data); break;
    STORAGE(RPE3R);    pps_output_group4(offset, data); break;
    STORAGE(RPE9R);    pps_output_group4(offset, data); break;
    STORAGE(RPF8R);    pps_output_group4(offset, data); break;
    STORAGE(RPF12R);   pps_output_group4(offset, data); break;
    STORAGE(RPG6R);    pps_output_group4(offset, data); break;

    /*-------------------------------------------------------------------------
     * Real-Time Clock and Calendar.
     */
    WRITEOPR(RTCCON, PIC32_RTCC_HALFSEC | PIC32_RTCC_SYNC | PIC32_RTCC_CLKON);
       if (VALUE(RTCCON) & PIC32_RTCC_ON)
          VALUE(RTCCON) = write_op(VALUE(RTCCON), PIC32_RTCC_CLKON, RTCCONSET);
       else
          VALUE(RTCCON) = write_op(VALUE(RTCCON), PIC32_RTCC_CLKON, RTCCONCLR);
       return;
    WRITEOP(RTCTIME); return;
    WRITEOP(RTCDATE); return;

    /*-------------------------------------------------------------------------
     * General purpose IO signals.
     */
    WRITEOP(ANSELA); return;        // Port A: analog select
    WRITEOP(TRISA); return;         // Port A: mask of inputs
    WRITEOPX(PORTA, LATA);          // Port A: write outputs
    WRITEOP(LATA);                  // Port A: write outputs
        pic32_gpio_write(s, 0, VALUE(LATA));
        return;
    WRITEOP(ODCA); return;          // Port A: open drain configuration
    WRITEOP(CNPUA); return;         // Input pin pull-up
    WRITEOP(CNPDA); return;         // Input pin pull-down
    WRITEOP(CNCONA); return;        // Interrupt-on-change control
    WRITEOP(CNENA); return;         // Input change interrupt enable
    WRITEOP(CNSTATA); return;       // Input change status

    WRITEOP(ANSELB); return;        // Port B: analog select
    WRITEOP(TRISB); return;         // Port B: mask of inputs
    WRITEOPX(PORTB, LATB);          // Port B: write outputs
    WRITEOP(LATB);                  // Port B: write outputs
        pic32_gpio_write(s, 1, VALUE(LATB));
        return;
    WRITEOP(ODCB); return;          // Port B: open drain configuration
    WRITEOP(CNPUB); return;         // Input pin pull-up
    WRITEOP(CNPDB); return;         // Input pin pull-down
    WRITEOP(CNCONB); return;        // Interrupt-on-change control
    WRITEOP(CNENB); return;         // Input change interrupt enable
    WRITEOP(CNSTATB); return;       // Input change status

    WRITEOP(ANSELC); return;        // Port C: analog select
    WRITEOP(TRISC); return;         // Port C: mask of inputs
    WRITEOPX(PORTC, LATC);          // Port C: write outputs
    WRITEOP(LATC);                  // Port C: write outputs
        pic32_gpio_write(s, 2, VALUE(LATC));
        return;
    WRITEOP(ODCC); return;          // Port C: open drain configuration
    WRITEOP(CNPUC); return;         // Input pin pull-up
    WRITEOP(CNPDC); return;         // Input pin pull-down
    WRITEOP(CNCONC); return;        // Interrupt-on-change control
    WRITEOP(CNENC); return;         // Input change interrupt enable
    WRITEOP(CNSTATC); return;       // Input change status

    WRITEOP(ANSELD); return;        // Port D: analog select
    WRITEOP(TRISD); return;         // Port D: mask of inputs
    WRITEOPX(PORTD, LATD);          // Port D: write outputs
    WRITEOP(LATD);                  // Port D: write outputs
        pic32_gpio_write(s, 3, VALUE(LATD));
        return;
    WRITEOP(ODCD); return;          // Port D: open drain configuration
    WRITEOP(CNPUD); return;         // Input pin pull-up
    WRITEOP(CNPDD); return;         // Input pin pull-down
    WRITEOP(CNCOND); return;        // Interrupt-on-change control
    WRITEOP(CNEND); return;         // Input change interrupt enable
    WRITEOP(CNSTATD); return;       // Input change status

    WRITEOP(ANSELE); return;        // Port E: analog select
    WRITEOP(TRISE); return;         // Port E: mask of inputs
    WRITEOPX(PORTE, LATE);          // Port E: write outputs
    WRITEOP(LATE);                  // Port E: write outputs
        pic32_gpio_write(s, 4, VALUE(LATE));
        return;
    WRITEOP(ODCE); return;          // Port E: open drain configuration
    WRITEOP(CNPUE); return;         // Input pin pull-up
    WRITEOP(CNPDE); return;         // Input pin pull-down
    WRITEOP(CNCONE); return;        // Interrupt-on-change control
    WRITEOP(CNENE); return;         // Input change interrupt enable
    WRITEOP(CNSTATE); return;       // Input change status

    WRITEOP(ANSELF); return;        // Port F: analog select
    WRITEOP(TRISF); return;         // Port F: mask of inputs
    WRITEOPX(PORTF, LATF);          // Port F: write outputs
    WRITEOP(LATF);                  // Port F: write outputs
        pic32_gpio_write(s, 5, VALUE(LATF));
        return;
    WRITEOP(ODCF); return;          // Port F: open drain configuration
    WRITEOP(CNPUF); return;         // Input pin pull-up
    WRITEOP(CNPDF); return;         // Input pin pull-down
    WRITEOP(CNCONF); return;        // Interrupt-on-change control
    WRITEOP(CNENF); return;         // Input change interrupt enable
    WRITEOP(CNSTATF); return;       // Input change status

    WRITEOP(ANSELG); return;        // Port G: analog select
    WRITEOP(TRISG); return;         // Port G: mask of inputs
    WRITEOPX(PORTG, LATG);          // Port G: write outputs
    WRITEOP(LATG);                  // Port G: write outputs
        pic32_gpio_write(s, 6, VALUE(LATG));
        return;
    WRITEOP(ODCG); return;          // Port G: open drain configuration
    WRITEOP(CNPUG); return;         // Input pin pull-up
    WRITEOP(CNPDG); return;         // Input pin pull-down
    WRITEOP(CNCONG); return;        // Interrupt-on-change control
    WRITEOP(CNENG); return;         // Input change interrupt enable
    WRITEOP(CNSTATG); return;       // Input change status

    WRITEOP(ANSELH); return;        // Port H: analog select
    WRITEOP(TRISH); return;         // Port H: mask of inputs
    WRITEOPX(PORTH, LATH);          // Port H: write outputs
    WRITEOP(LATH);                  // Port H: write outputs
        pic32_gpio_write(s, 7, VALUE(LATH));
        return;
    WRITEOP(ODCH); return;          // Port H: open drain configuration
    WRITEOP(CNPUH); return;         // Input pin pull-up
    WRITEOP(CNPDH); return;         // Input pin pull-down
    WRITEOP(CNCONH); return;        // Interrupt-on-change control
    WRITEOP(CNENH); return;         // Input change interrupt enable
    WRITEOP(CNSTATH); return;       // Input change status

    WRITEOP(ANSELJ); return;        // Port J: analog select
    WRITEOP(TRISJ); return;         // Port J: mask of inputs
    WRITEOPX(PORTJ, LATJ);          // Port J: write outputs
    WRITEOP(LATJ);                  // Port J: write outputs
        pic32_gpio_write(s, 8, VALUE(LATJ));
        return;
    WRITEOP(ODCJ); return;          // Port J: open drain configuration
    WRITEOP(CNPUJ); return;         // Input pin pull-up
    WRITEOP(CNPDJ); return;         // Input pin pull-down
    WRITEOP(CNCONJ); return;        // Interrupt-on-change control
    WRITEOP(CNENJ); return;         // Input change interrupt enable
    WRITEOP(CNSTATJ); return;       // Input change status

    WRITEOP(TRISK); return;         // Port K: mask of inputs
    WRITEOPX(PORTK, LATK);          // Port K: write outputs
    WRITEOP(LATK);                  // Port K: write outputs
        pic32_gpio_write(s, 9, VALUE(LATK));
        return;
    WRITEOP(ODCK); return;          // Port K: open drain configuration
    WRITEOP(CNPUK); return;         // Input pin pull-up
    WRITEOP(CNPDK); return;         // Input pin pull-down
    WRITEOP(CNCONK); return;        // Interrupt-on-change control
    WRITEOP(CNENK); return;         // Input change interrupt enable
    WRITEOP(CNSTATK); return;       // Input change status

    /*-------------------------------------------------------------------------
     * UART 1.
     */
    STORAGE(U1TXREG);                               // Transmit
        pic32_uart_put_char(s, 0, data);
        break;
    WRITEOP(U1MODE);                                // Mode
        pic32_uart_update_mode(s, 0);
        return;
    WRITEOPR(U1STA,                                 // Status and control
        PIC32_USTA_URXDA | PIC32_USTA_FERR | PIC32_USTA_PERR |
        PIC32_USTA_RIDLE | PIC32_USTA_TRMT | PIC32_USTA_UTXBF);
        pic32_uart_update_status(s, 0);
        return;
    WRITEOP(U1BRG); return;                         // Baud rate
    READONLY(U1RXREG);                              // Receive

    /*-------------------------------------------------------------------------
     * UART 2.
     */
    STORAGE(U2TXREG);                               // Transmit
        pic32_uart_put_char(s, 1, data);
        break;
    WRITEOP(U2MODE);                                // Mode
        pic32_uart_update_mode(s, 1);
        return;
    WRITEOPR(U2STA,                                 // Status and control
        PIC32_USTA_URXDA | PIC32_USTA_FERR | PIC32_USTA_PERR |
        PIC32_USTA_RIDLE | PIC32_USTA_TRMT | PIC32_USTA_UTXBF);
        pic32_uart_update_status(s, 1);
        return;
    WRITEOP(U2BRG); return;                         // Baud rate
    READONLY(U2RXREG);                              // Receive

    /*-------------------------------------------------------------------------
     * UART 3.
     */
    STORAGE(U3TXREG);                               // Transmit
        pic32_uart_put_char(s, 2, data);
        break;
    WRITEOP(U3MODE);                                // Mode
        pic32_uart_update_mode(s, 2);
        return;
    WRITEOPR(U3STA,                                 // Status and control
        PIC32_USTA_URXDA | PIC32_USTA_FERR | PIC32_USTA_PERR |
        PIC32_USTA_RIDLE | PIC32_USTA_TRMT | PIC32_USTA_UTXBF);
        pic32_uart_update_status(s, 2);
        return;
    WRITEOP(U3BRG); return;                         // Baud rate
    READONLY(U3RXREG);                              // Receive

    /*-------------------------------------------------------------------------
     * UART 4.
     */
    STORAGE(U4TXREG);                               // Transmit
        pic32_uart_put_char(s, 3, data);
        break;
    WRITEOP(U4MODE);                                // Mode
        pic32_uart_update_mode(s, 3);
        return;
    WRITEOPR(U4STA,                                 // Status and control
        PIC32_USTA_URXDA | PIC32_USTA_FERR | PIC32_USTA_PERR |
        PIC32_USTA_RIDLE | PIC32_USTA_TRMT | PIC32_USTA_UTXBF);
        pic32_uart_update_status(s, 3);
        return;
    WRITEOP(U4BRG); return;                         // Baud rate
    READONLY(U4RXREG);                              // Receive

    /*-------------------------------------------------------------------------
     * UART 5.
     */
    STORAGE(U5TXREG);                               // Transmit
        pic32_uart_put_char(s, 4, data);
        break;
    WRITEOP(U5MODE);                                // Mode
        pic32_uart_update_mode(s, 4);
        return;
    WRITEOPR(U5STA,                                 // Status and control
        PIC32_USTA_URXDA | PIC32_USTA_FERR | PIC32_USTA_PERR |
        PIC32_USTA_RIDLE | PIC32_USTA_TRMT | PIC32_USTA_UTXBF);
        pic32_uart_update_status(s, 4);
        return;
    WRITEOP(U5BRG); return;                         // Baud rate
    READONLY(U5RXREG);                              // Receive

    /*-------------------------------------------------------------------------
     * UART 6.
     */
    STORAGE(U6TXREG);                               // Transmit
        pic32_uart_put_char(s, 5, data);
        break;
    WRITEOP(U6MODE);                                // Mode
        pic32_uart_update_mode(s, 5);
        return;
    WRITEOPR(U6STA,                                 // Status and control
        PIC32_USTA_URXDA | PIC32_USTA_FERR | PIC32_USTA_PERR |
        PIC32_USTA_RIDLE | PIC32_USTA_TRMT | PIC32_USTA_UTXBF);
        pic32_uart_update_status(s, 5);
        return;
    WRITEOP(U6BRG); return;                         // Baud rate
    READONLY(U6RXREG);                              // Receive

    /*-------------------------------------------------------------------------
     * SPI.
     */
    WRITEOP(SPI1CON);                               // Control
        pic32_spi_control(s, 0);
        return;
    WRITEOPR(SPI1STAT, ~PIC32_SPISTAT_SPIROV);      // Status
        return;                                     // Only ROV bit is writable
    STORAGE(SPI1BUF);                               // Buffer
        pic32_spi_writebuf(s, 0, data);
        return;
    WRITEOP(SPI1BRG); return;                       // Baud rate
    WRITEOP(SPI1CON2); return;                      // Control 2

    WRITEOP(SPI2CON);                               // Control
        pic32_spi_control(s, 1);
        return;
    WRITEOPR(SPI2STAT, ~PIC32_SPISTAT_SPIROV);      // Status
        return;                                     // Only ROV bit is writable
    STORAGE(SPI2BUF);                               // Buffer
        pic32_spi_writebuf(s, 1, data);
        return;
    WRITEOP(SPI2BRG); return;                       // Baud rate
    WRITEOP(SPI2CON2); return;                      // Control 2

    WRITEOP(SPI3CON);                               // Control
        pic32_spi_control(s, 2);
        return;
    WRITEOPR(SPI3STAT, ~PIC32_SPISTAT_SPIROV);      // Status
        return;                                     // Only ROV bit is writable
    STORAGE(SPI3BUF);                               // Buffer
        pic32_spi_writebuf(s, 2, data);
        return;
    WRITEOP(SPI3BRG); return;                       // Baud rate
    WRITEOP(SPI3CON2); return;                      // Control 2

    WRITEOP(SPI4CON);                               // Control
        pic32_spi_control(s, 3);
        return;
    WRITEOPR(SPI4STAT, ~PIC32_SPISTAT_SPIROV);      // Status
        return;                                     // Only ROV bit is writable
    STORAGE(SPI4BUF);                               // Buffer
        pic32_spi_writebuf(s, 3, data);
        return;
    WRITEOP(SPI4BRG); return;                       // Baud rate
    WRITEOP(SPI4CON2); return;                      // Control 2

    /*
     * Timers 1-9.
     */
    WRITEOP(T1CON); return;
    WRITEOP(TMR1); return;
    WRITEOP(PR1); return;
    WRITEOP(T2CON); return;
    WRITEOP(TMR2); return;
    WRITEOP(PR2); return;
    WRITEOP(T3CON); return;
    WRITEOP(TMR3); return;
    WRITEOP(PR3); return;
    WRITEOP(T4CON); return;
    WRITEOP(TMR4); return;
    WRITEOP(PR4); return;
    WRITEOP(T5CON); return;
    WRITEOP(TMR5); return;
    WRITEOP(PR5); return;
    WRITEOP(T6CON); return;
    WRITEOP(TMR6); return;
    WRITEOP(PR6); return;
    WRITEOP(T7CON); return;
    WRITEOP(TMR7); return;
    WRITEOP(PR7); return;
    WRITEOP(T8CON); return;
    WRITEOP(TMR8); return;
    WRITEOP(PR8); return;
    WRITEOP(T9CON); return;
    WRITEOP(TMR9); return;
    WRITEOP(PR9); return;

    /*-------------------------------------------------------------------------
     * Ethernet.
     */
    WRITEOP(ETHCON1);                   // Control 1
        pic32_eth_control(s);
        return;
    WRITEOP(ETHCON2); return;           // Control 2: RX data buffer size
    WRITEOP(ETHTXST); return;           // Tx descriptor start address
    WRITEOP(ETHRXST); return;           // Rx descriptor start address
    WRITEOP(ETHHT0); return;            // Hash tasble 0
    WRITEOP(ETHHT1); return;            // Hash tasble 1
    WRITEOP(ETHPMM0); return;           // Pattern match mask 0
    WRITEOP(ETHPMM1); return;           // Pattern match mask 1
    WRITEOP(ETHPMCS); return;           // Pattern match checksum
    WRITEOP(ETHPMO); return;            // Pattern match offset
    WRITEOP(ETHRXFC); return;           // Receive filter configuration
    WRITEOP(ETHRXWM); return;           // Receive watermarks
    WRITEOP(ETHIEN); return;            // Interrupt enable
    WRITEOP(ETHIRQ); return;            // Interrupt request
    STORAGE(ETHSTAT); break;            // Status
    WRITEOP(ETHRXOVFLOW); return;       // Receive overflow statistics
    WRITEOP(ETHFRMTXOK); return;        // Frames transmitted OK statistics
    WRITEOP(ETHSCOLFRM); return;        // Single collision frames statistics
    WRITEOP(ETHMCOLFRM); return;        // Multiple collision frames statistics
    WRITEOP(ETHFRMRXOK); return;        // Frames received OK statistics
    WRITEOP(ETHFCSERR); return;         // Frame check sequence error statistics
    WRITEOP(ETHALGNERR); return;        // Alignment errors statistics
    WRITEOP(EMAC1CFG1); return;         // MAC configuration 1
    WRITEOP(EMAC1CFG2); return;         // MAC configuration 2
    WRITEOP(EMAC1IPGT); return;         // MAC back-to-back interpacket gap
    WRITEOP(EMAC1IPGR); return;         // MAC non-back-to-back interpacket gap
    WRITEOP(EMAC1CLRT); return;         // MAC collision window/retry limit
    WRITEOP(EMAC1MAXF); return;         // MAC maximum frame length
    WRITEOP(EMAC1SUPP); return;         // MAC PHY support
    WRITEOP(EMAC1TEST); return;         // MAC test
    WRITEOP(EMAC1MCFG); return;         // MII configuration
    WRITEOP(EMAC1MCMD);                 // MII command
        pic32_mii_command(s);
        return;
    WRITEOP(EMAC1MADR); return;         // MII address
    WRITEOP(EMAC1MWTD);                 // MII write data
        pic32_mii_write(s);
        return;
    WRITEOP(EMAC1MRDD); return;         // MII read data
    WRITEOP(EMAC1MIND); return;         // MII indicators
    WRITEOP(EMAC1SA0); return;          // MAC station address 0
    WRITEOP(EMAC1SA1); return;          // MAC station address 1
    WRITEOP(EMAC1SA2); return;          // MAC station address 2

    /*-------------------------------------------------------------------------
     * USB.
     */
    STORAGE(USBCSR0); break;
    STORAGE(USBCSR1); break;
    STORAGE(USBCSR2); break;
    STORAGE(USBCSR3); break;
    STORAGE(USBIENCSR0); break;
    STORAGE(USBIENCSR1); break;
    STORAGE(USBIENCSR2); break;
    STORAGE(USBIENCSR3); break;
    STORAGE(USBFIFO0); break;
    STORAGE(USBFIFO1); break;
    STORAGE(USBFIFO2); break;
    STORAGE(USBFIFO3); break;
    STORAGE(USBFIFO4); break;
    STORAGE(USBFIFO5); break;
    STORAGE(USBFIFO6); break;
    STORAGE(USBFIFO7); break;
    STORAGE(USBOTG); break;
    STORAGE(USBFIFOA); break;
    STORAGE(USBHWVER); break;
    STORAGE(USBINFO); break;
    STORAGE(USBEOFRST); break;
    STORAGE(USBE0TXA); break;
    STORAGE(USBE0RXA); break;
    STORAGE(USBE1TXA); break;
    STORAGE(USBE1RXA); break;
    STORAGE(USBE2TXA); break;
    STORAGE(USBE2RXA); break;
    STORAGE(USBE3TXA); break;
    STORAGE(USBE3RXA); break;
    STORAGE(USBE4TXA); break;
    STORAGE(USBE4RXA); break;
    STORAGE(USBE5TXA); break;
    STORAGE(USBE5RXA); break;
    STORAGE(USBE6TXA); break;
    STORAGE(USBE6RXA); break;
    STORAGE(USBE7TXA); break;
    STORAGE(USBE7RXA); break;
    STORAGE(USBE0CSR0); break;
    STORAGE(USBE0CSR2); break;
    STORAGE(USBE0CSR3); break;
    STORAGE(USBE1CSR0); break;
    STORAGE(USBE1CSR1); break;
    STORAGE(USBE1CSR2); break;
    STORAGE(USBE1CSR3); break;
    STORAGE(USBE2CSR0); break;
    STORAGE(USBE2CSR1); break;
    STORAGE(USBE2CSR2); break;
    STORAGE(USBE2CSR3); break;
    STORAGE(USBE3CSR0); break;
    STORAGE(USBE3CSR1); break;
    STORAGE(USBE3CSR2); break;
    STORAGE(USBE3CSR3); break;
    STORAGE(USBE4CSR0); break;
    STORAGE(USBE4CSR1); break;
    STORAGE(USBE4CSR2); break;
    STORAGE(USBE4CSR3); break;
    STORAGE(USBE5CSR0); break;
    STORAGE(USBE5CSR1); break;
    STORAGE(USBE5CSR2); break;
    STORAGE(USBE5CSR3); break;
    STORAGE(USBE6CSR0); break;
    STORAGE(USBE6CSR1); break;
    STORAGE(USBE6CSR2); break;
    STORAGE(USBE6CSR3); break;
    STORAGE(USBE7CSR0); break;
    STORAGE(USBE7CSR1); break;
    STORAGE(USBE7CSR2); break;
    STORAGE(USBE7CSR3); break;
    STORAGE(USBDMAINT); break;
    STORAGE(USBDMA1C); break;
    STORAGE(USBDMA1A); break;
    STORAGE(USBDMA1N); break;
    STORAGE(USBDMA2C); break;
    STORAGE(USBDMA2A); break;
    STORAGE(USBDMA2N); break;
    STORAGE(USBDMA3C); break;
    STORAGE(USBDMA3A); break;
    STORAGE(USBDMA3N); break;
    STORAGE(USBDMA4C); break;
    STORAGE(USBDMA4A); break;
    STORAGE(USBDMA4N); break;
    STORAGE(USBDMA5C); break;
    STORAGE(USBDMA5A); break;
    STORAGE(USBDMA5N); break;
    STORAGE(USBDMA6C); break;
    STORAGE(USBDMA6A); break;
    STORAGE(USBDMA6N); break;
    STORAGE(USBDMA7C); break;
    STORAGE(USBDMA7A); break;
    STORAGE(USBDMA7N); break;
    STORAGE(USBDMA8C); break;
    STORAGE(USBDMA8A); break;
    STORAGE(USBDMA8N); break;
    STORAGE(USBE1RPC); break;
    STORAGE(USBE2RPC); break;
    STORAGE(USBE3RPC); break;
    STORAGE(USBE4RPC); break;
    STORAGE(USBE5RPC); break;
    STORAGE(USBE6RPC); break;
    STORAGE(USBE7RPC); break;
    STORAGE(USBDPBFD); break;
    STORAGE(USBTMCON1); break;
    STORAGE(USBTMCON2); break;
    STORAGE(USBLPMR1); break;
    STORAGE(USBLMPR2); break;

    default:
        printf("--- Write %08x to 1f8%05x: peripheral register not supported\n",
            data, offset);
        if (qemu_loglevel_mask(CPU_LOG_INSTR))
            fprintf(qemu_logfile, "--- Write %08x to 1f8%05x: peripheral register not supported\n",
                data, offset);
        exit(1);
readonly:
        printf("--- Write %08x to %s: readonly register\n",
            data, *namep);
        if (qemu_loglevel_mask(CPU_LOG_INSTR))
            fprintf(qemu_logfile, "--- Write %08x to %s: readonly register\n",
                data, *namep);
        *namep = 0;
        return;
    }
    *bufp = data;
}

static uint64_t pic32_io_read(void *opaque, hwaddr addr, unsigned bytes)
{
    pic32_t *s = opaque;
    uint32_t offset = addr & 0xfffff;
    const char *name = "???";
    uint32_t data = 0;

    data = io_read32(s, offset & ~3, &name);
    switch (bytes) {
    case 1:
        if ((offset &= 3) != 0) {
            // Unaligned read.
            data >>= offset * 8;
        }
        data = (uint8_t) data;
        if (qemu_loglevel_mask(CPU_LOG_INSTR)) {
            fprintf(qemu_logfile, "--- I/O Read  %02x from %s\n", data, name);
        }
        break;
    case 2:
        if (offset & 2) {
            // Unaligned read.
            data >>= 16;
        }
        data = (uint16_t) data;
        if (qemu_loglevel_mask(CPU_LOG_INSTR)) {
            fprintf(qemu_logfile, "--- I/O Read  %04x from %s\n", data, name);
        }
        break;
    default:
        if (qemu_loglevel_mask(CPU_LOG_INSTR)) {
            fprintf(qemu_logfile, "--- I/O Read  %08x from %s\n", data, name);
        }
        break;
    }
    return data;
}

static void pic32_io_write(void *opaque, hwaddr addr, uint64_t data, unsigned bytes)
{
    pic32_t *s = opaque;
    uint32_t offset = addr & 0xfffff;
    const char *name = "???";

    // Fetch data and align to word format.
    switch (bytes) {
    case 1:
        data = (uint8_t) data;
        data <<= (offset & 3) * 8;
        break;
    case 2:
        data = (uint16_t) data;
        data <<= (offset & 2) * 8;
        break;
    }
    io_write32(s, offset & ~3, data, &name);

    if (qemu_loglevel_mask(CPU_LOG_INSTR) && name != 0) {
        fprintf(qemu_logfile, "--- I/O Write %08x to %s\n",
            (uint32_t) data, name);
    }
}

static const MemoryRegionOps pic32_io_ops = {
    .read       = pic32_io_read,
    .write      = pic32_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void main_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;
    int i;

    cpu_reset(CPU(cpu));

    /* Adjust the initial configuration for microAptivP core. */
    env->CP0_IntCtl = 0x00030000;
    env->CP0_Debug = (1 << CP0DB_CNT) | (5 << CP0DB_VER);
    env->CP0_Performance0 = 0x80000000;
    for (i=0; i<7; i++)
        env->CP0_WatchHi[i] = (i < 3) ? 0x80000000 : 0;
}

static void store_byte(unsigned address, unsigned char byte)
{
    if (address >= PROGRAM_FLASH_START &&
        address < PROGRAM_FLASH_START + PROGRAM_FLASH_SIZE)
    {
        //printf("Store %02x to program memory %08x\n", byte, address);
        prog_ptr[address & 0xfffff] = byte;
    }
    else if (address >= BOOT_FLASH_START &&
             address < BOOT_FLASH_START + BOOT_FLASH_SIZE)
    {
        //printf("Store %02x to boot memory %08x\n", byte, address);
        boot_ptr[address & 0xffff] = byte;
    }
    else {
        printf("Bad hex file: incorrect address %08X, must be %08X-%08X or %08X-%08X\n",
            address, PROGRAM_FLASH_START,
            PROGRAM_FLASH_START + PROGRAM_FLASH_SIZE - 1,
            BOOT_FLASH_START, BOOT_FLASH_START + BOOT_FLASH_SIZE - 1);
        exit(1);
    }
}

/*
 * Ignore ^C and ^\ signals and pass these characters to the target.
 */
static void pic32_pass_signal_chars(void)
{
    struct termios tty;

    tcgetattr(0, &tty);
    tty.c_lflag &= ~ISIG;
    tcsetattr(0, TCSANOW, &tty);
}

static void pic32_init(MachineState *machine, int board_type)
{
    const char *cpu_model = machine->cpu_model;
    unsigned ram_size = DATA_MEM_SIZE;
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *ram_main = g_new(MemoryRegion, 1);
    MemoryRegion *prog_mem = g_new(MemoryRegion, 1);
    MemoryRegion *boot_mem = g_new(MemoryRegion, 1);
    MemoryRegion *io_mem = g_new(MemoryRegion, 1);
    MIPSCPU *cpu;
    CPUMIPSState *env;

    DeviceState *dev = qdev_create(NULL, TYPE_MIPS_PIC32);
    pic32_t *s = OBJECT_CHECK(pic32_t, dev, TYPE_MIPS_PIC32);
    s->board_type = board_type;
    s->stop_on_reset = 1;               /* halt simulation on soft reset */
    s->iomem = g_malloc0(IO_MEM_SIZE);  /* backing storage for I/O area */

    qdev_init_nofail(dev);

    /* Init CPU. */
    if (! cpu_model) {
        cpu_model = "microAptivP";
    }
    printf("Board: %s\n", board_name[board_type]);
    if (qemu_logfile)
        fprintf(qemu_logfile, "Board: %s\n", board_name[board_type]);

    printf("Processor: %s\n", cpu_model);
    if (qemu_logfile)
        fprintf(qemu_logfile, "Processor: %s\n", cpu_model);

    cpu = cpu_mips_init(cpu_model);
    if (! cpu) {
        fprintf(stderr, "Unable to find CPU definition\n");
        exit(1);
    }
    s->cpu = cpu;
    env = &cpu->env;

    /* Register RAM */
    printf("RAM size: %u kbytes\n", ram_size / 1024);
    if (qemu_logfile)
        fprintf(qemu_logfile, "RAM size: %u kbytes\n", ram_size / 1024);

    memory_region_init_ram(ram_main, NULL, "kernel.ram",
        ram_size, &error_abort);
    vmstate_register_ram_global(ram_main);
    memory_region_add_subregion(system_memory, DATA_MEM_START, ram_main);

    /* Special function registers. */
    memory_region_init_io(io_mem, NULL, &pic32_io_ops, s,
                          "io", IO_MEM_SIZE);
    memory_region_add_subregion(system_memory, IO_MEM_START, io_mem);

    /*
     * Map the flash memory.
     */
    memory_region_init_ram(boot_mem, NULL, "boot.flash", BOOT_FLASH_SIZE, &error_abort);
    memory_region_init_ram(prog_mem, NULL, "prog.flash", PROGRAM_FLASH_SIZE, &error_abort);

    /* Load a Flash memory image. */
    if (! machine->kernel_filename) {
        error_report("No -kernel argument was specified.");
        exit(1);
    }
    prog_ptr = memory_region_get_ram_ptr(prog_mem);
    boot_ptr = memory_region_get_ram_ptr(boot_mem);
    if (bios_name)
        pic32_load_hex_file(bios_name, store_byte);
    pic32_load_hex_file(machine->kernel_filename, store_byte);

    memory_region_set_readonly(boot_mem, true);
    memory_region_set_readonly(prog_mem, true);
    memory_region_add_subregion(system_memory, BOOT_FLASH_START, boot_mem);
    memory_region_add_subregion(system_memory, PROGRAM_FLASH_START, prog_mem);

    /* Init internal devices */
    s->irq_raise = irq_raise;
    s->irq_clear = irq_clear;
    qemu_register_reset(main_cpu_reset, cpu);

    /* Setup interrupt controller in EIC mode. */
    env->CP0_Config3 |= 1 << CP0C3_VEIC;
    cpu_mips_irq_init_cpu(env);
    env->eic_timer_irq = pic32_timer_irq;
    env->eic_soft_irq = pic32_soft_irq;
    env->eic_context = s;

    /* CPU runs at 200MHz.
     * Count register increases at half this rate. */
    cpu_mips_clock_init(env, 100*1000*1000);

    /*
     * Initialize board-specific parameters.
     */
    int cs0_port, cs0_pin, cs1_port, cs1_pin;
    switch (board_type) {
    default:
    case BOARD_WIFIRE:                      // console on UART4
        BOOTMEM(DEVCFG0) = 0xfffffff7;
        BOOTMEM(DEVCFG1) = 0x7f743cb9;
        BOOTMEM(DEVCFG2) = 0xfff9b11a;
        BOOTMEM(DEVCFG3) = 0xbeffffff;
        VALUE(DEVID)     = 0x4510e053;      // MZ2048ECG100 rev A4
        VALUE(OSCCON)    = 0x00001120;      // external oscillator 24MHz
        s->sdcard_spi_port = 2;             // SD card at SPI3,
        cs0_port = 2;  cs0_pin = 3;         // select0 at C3,
        cs1_port = -1; cs1_pin = -1;        // select1 not available
        break;
    case BOARD_MEBII:                       // console on UART1
        BOOTMEM(DEVCFG0) = 0x7fffffdb;
        BOOTMEM(DEVCFG1) = 0x0000fc81;
        BOOTMEM(DEVCFG2) = 0x3ff8b11a;
        BOOTMEM(DEVCFG3) = 0x86ffffff;
        VALUE(DEVID)     = 0x45127053;      // MZ2048ECH144 rev A4
        VALUE(OSCCON)    = 0x00001120;      // external oscillator 24MHz
        s->sdcard_spi_port = 1;             // SD card at SPI2,
        cs0_port = 1;  cs0_pin = 14;        // select0 at B14,
        cs1_port = -1; cs1_pin = -1;        // select1 not available
        break;
    case BOARD_EXPLORER16:                  // console on UART1
        BOOTMEM(DEVCFG0) = 0x7fffffdb;
        BOOTMEM(DEVCFG1) = 0x0000fc81;
        BOOTMEM(DEVCFG2) = 0x3ff8b11a;
        BOOTMEM(DEVCFG3) = 0x86ffffff;
        VALUE(DEVID)     = 0x35113053;      // MZ2048ECH100 rev A3
        VALUE(OSCCON)    = 0x00001120;      // external oscillator 24MHz
        s->sdcard_spi_port = 0;             // SD card at SPI1,
        cs0_port = 1;  cs0_pin = 1;         // select0 at B1,
        cs1_port = 1;  cs1_pin = 2;         // select1 at B2
        break;
    case BOARD_HMZ144:                      // console on UART2
        BOOTMEM(DEVCFG0) = 0x7fffffdb;
        BOOTMEM(DEVCFG1) = 0x0000bec1;
        BOOTMEM(DEVCFG2) = 0x3ff8e31a;
        BOOTMEM(DEVCFG3) = 0x86ffffff;
        VALUE(DEVID)     = 0x55122053;      // MZ2048ECG144 rev A5
        VALUE(OSCCON)    = 0x00001122;      // external oscillator 12MHz
        s->sdcard_spi_port = 1;             // SD card at SPI2,
        cs0_port = 1;  cs0_pin = 14;        // select0 at B14,
        cs1_port = -1; cs1_pin = -1;        // select1 not available
        break;
    }

    /* UARTs */
    pic32_uart_init(s, 0, PIC32_IRQ_U1E, U1STA, U1MODE);
    pic32_uart_init(s, 1, PIC32_IRQ_U2E, U2STA, U2MODE);
    pic32_uart_init(s, 2, PIC32_IRQ_U3E, U3STA, U3MODE);
    pic32_uart_init(s, 3, PIC32_IRQ_U4E, U4STA, U4MODE);
    pic32_uart_init(s, 4, PIC32_IRQ_U5E, U5STA, U5MODE);
    pic32_uart_init(s, 5, PIC32_IRQ_U6E, U6STA, U6MODE);

    /* SPIs */
    pic32_spi_init(s, 0, PIC32_IRQ_SPI1E, SPI1CON, SPI1STAT);
    pic32_spi_init(s, 1, PIC32_IRQ_SPI2E, SPI2CON, SPI2STAT);
    pic32_spi_init(s, 2, PIC32_IRQ_SPI3E, SPI3CON, SPI3STAT);
    pic32_spi_init(s, 3, PIC32_IRQ_SPI4E, SPI4CON, SPI4STAT);
    pic32_spi_init(s, 4, PIC32_IRQ_SPI5E, SPI5CON, SPI5STAT);
    pic32_spi_init(s, 5, PIC32_IRQ_SPI6E, SPI6CON, SPI6STAT);

    /*
     * Load SD card images.
     * Use options:
     *      -sd filename
     * or   -hda filename
     * and  -hdb filename
     */
    const char *sd0_file = 0, *sd1_file = 0;
    DriveInfo *dinfo = drive_get(IF_IDE, 0, 0);
    if (dinfo) {
        sd0_file = qemu_opt_get(dinfo->opts, "file");
        dinfo->is_default = 1;

        dinfo = drive_get(IF_IDE, 0, 1);
        if (dinfo) {
            sd1_file = qemu_opt_get(dinfo->opts, "file");
            dinfo->is_default = 1;
        }
    }
    if (! sd0_file) {
        dinfo = drive_get(IF_SD, 0, 0);
        if (dinfo) {
            sd0_file = qemu_opt_get(dinfo->opts, "file");
            dinfo->is_default = 1;
        }
    }
    pic32_sdcard_init(s, 0, "sd0", sd0_file, cs0_port, cs0_pin);
    pic32_sdcard_init(s, 1, "sd1", sd1_file, cs1_port, cs1_pin);

    /* Ethernet. */
    if (nd_table[0].used)
        pic32_eth_init(s, &nd_table[0]);

    io_reset(s);
    pic32_sdcard_reset(s);
    pic32_pass_signal_chars();
}

static void pic32_init_wifire(MachineState *machine)
{
    pic32_init(machine, BOARD_WIFIRE);
}

static void pic32_init_meb2(MachineState *machine)
{
    pic32_init(machine, BOARD_MEBII);
}

static void pic32_init_explorer16(MachineState *machine)
{
    pic32_init(machine, BOARD_EXPLORER16);
}

static void pic32_init_hmz144(MachineState *machine)
{
    pic32_init(machine, BOARD_HMZ144);
}

static int pic32_sysbus_device_init(SysBusDevice *sysbusdev)
{
    return 0;
}

static void pic32_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = pic32_sysbus_device_init;
}

static const TypeInfo pic32_device = {
    .name          = TYPE_MIPS_PIC32,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(pic32_t),
    .class_init    = pic32_class_init,
};

static void pic32_register_types(void)
{
    type_register_static(&pic32_device);
}

static QEMUMachine pic32_board[] = {
    {
        .name       = "pic32mz-wifire",
        .desc       = "PIC32MZ microcontroller on chipKIT WiFire board",
        .init       = pic32_init_wifire,
        .max_cpus   = 1,
    },
    {
        .name       = "pic32mz-meb2",
        .desc       = "PIC32MZ microcontroller on Microchip MEB-II board",
        .init       = pic32_init_meb2,
        .max_cpus   = 1,
    },
    {
        .name       = "pic32mz-explorer16",
        .desc       = "PIC32MZ microcontroller on Microchip Explorer-16 board",
        .init       = pic32_init_explorer16,
        .max_cpus   = 1,
    },
    {
        .name       = "pic32mz-hmz144",
        .desc       = "PIC32MZ microcontroller on Olimex HMZ144 board",
        .init       = pic32_init_hmz144,
        .max_cpus   = 1,
    },
};

static void pic32_machine_init(void)
{
    qemu_register_machine(&pic32_board[0]);
    qemu_register_machine(&pic32_board[1]);
    qemu_register_machine(&pic32_board[2]);
    qemu_register_machine(&pic32_board[3]);
}

type_init(pic32_register_types)
machine_init(pic32_machine_init);

#endif /* !TARGET_MIPS64 && !TARGET_WORDS_BIGENDIAN */
