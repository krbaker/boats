// Rafael Micro R820T/R820T2 tuner (RTL-SDR Blog V3), ported from librtlsdr's
// tuner_r82xx.c. GPL-2.0-or-later, see r82xx.c.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define R82XX_NUM_REGS 30 // shadowed registers 0x05..0x22

typedef struct {
    // I2C through the RTL2832U's repeater, which the caller turns on. Both
    // return the number of bytes moved, or a negative number on failure.
    int (*i2c_write)(uint8_t addr, const uint8_t *buf, int len);
    int (*i2c_read)(uint8_t addr, uint8_t *buf, int len);
    uint8_t i2c_addr;
    uint32_t xtal_hz;

    uint8_t regs[R82XX_NUM_REGS];
    uint8_t buf[R82XX_NUM_REGS + 1];
    uint8_t fil_cal_code;
    bool has_lock;
    uint32_t int_freq; // IF the tuner outputs, which the RTL2832U mixes back down
} r82xx_t;

int r82xx_init(r82xx_t *t);
// Sets the IF filter for this sample rate. Returns the new IF in Hz (to give to
// the RTL2832U), or a negative number on failure.
int r82xx_set_bandwidth(r82xx_t *t, uint32_t bw_hz);
// Tunes to freq_hz. Call after r82xx_set_bandwidth. Fails if the PLL won't lock.
int r82xx_set_freq(r82xx_t *t, uint32_t freq_hz);
// Manual gain in tenths of a dB (rounded up to the next step), or the tuner's AGC.
int r82xx_set_gain(r82xx_t *t, bool manual, int tenth_db);
