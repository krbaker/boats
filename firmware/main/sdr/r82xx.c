/*
 * Rafael Micro R820T/R820T2 tuner driver, ported from librtlsdr's
 * tuner_r82xx.c (rtl-sdr-blog fork), trimmed to what the RTL-SDR Blog V3 uses:
 * no R828D, no Blog V4 input switching, no xtal capacitor search.
 *
 * Copyright (C) 2013 Mauro Carvalho Chehab <mchehab@redhat.com>
 * Copyright (C) 2013 Steve Markgraf <steve@steve-m.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */

#include "r82xx.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "r82xx";

#define REG_SHADOW_START 5
#define VER_NUM          49
#define MAX_I2C_MSG_LEN  8
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))

// Initial values for registers 0x05..0x22.
static const uint8_t k_init_array[R82XX_NUM_REGS] = {
    0x83, 0x30, 0x75,                   // 05 to 07
    0xc0, 0x40, 0xd6, 0x6c,             // 08 to 0b
    0xf5, 0x63, 0x75, 0x68,             // 0c to 0f
    0x6c, 0x83, 0x80, 0x00,             // 10 to 13
    0x0f, 0x00, 0xc0, 0x30,             // 14 to 17
    0x48, 0xcc, 0x60, 0x00,             // 18 to 1b
    0x54, 0xae, 0x4a, 0xc0,             // 1c to 1f
};

// Per-band RF settings. The xtal cap column is the "high cap 0pF" one librtlsdr
// always selects for the R820T.
static const struct {
    uint16_t mhz;       // band start
    uint8_t open_d;     // R23[3]
    uint8_t rf_mux;     // R26[7:6] filter, R26[1:0] polymux
    uint8_t tf_c;       // R27 tracking filter
    uint8_t xtal_cap;   // R16[1:0]
} k_ranges[] = {
    {0, 0x08, 0x02, 0xdf, 0x00},   {50, 0x08, 0x02, 0xbe, 0x00},  {55, 0x08, 0x02, 0x8b, 0x00},
    {60, 0x08, 0x02, 0x7b, 0x00},  {65, 0x08, 0x02, 0x69, 0x00},  {70, 0x08, 0x02, 0x58, 0x00},
    {75, 0x00, 0x02, 0x44, 0x00},  {80, 0x00, 0x02, 0x44, 0x00},  {90, 0x00, 0x02, 0x34, 0x00},
    {100, 0x00, 0x02, 0x34, 0x00}, {110, 0x00, 0x02, 0x24, 0x00}, {120, 0x00, 0x02, 0x24, 0x00},
    {140, 0x00, 0x02, 0x14, 0x00}, {180, 0x00, 0x02, 0x13, 0x00}, {220, 0x00, 0x02, 0x13, 0x00},
    {250, 0x00, 0x02, 0x11, 0x00}, {280, 0x00, 0x02, 0x00, 0x00}, {310, 0x00, 0x41, 0x00, 0x00},
    {450, 0x00, 0x41, 0x00, 0x00}, {588, 0x00, 0x40, 0x00, 0x00}, {650, 0x00, 0x40, 0x00, 0x00},
};

// ---------------------------------------------------------------------------
// I2C and shadow registers
// ---------------------------------------------------------------------------

static void shadow_store(r82xx_t *t, uint8_t reg, const uint8_t *val, int len)
{
    int r = reg - REG_SHADOW_START;
    if (r < 0) {
        len += r;
        val -= r;
        r = 0;
    }
    if (len <= 0) return;
    if (len > R82XX_NUM_REGS - r) len = R82XX_NUM_REGS - r;
    memcpy(&t->regs[r], val, len);
}

static int write_regs(r82xx_t *t, uint8_t reg, const uint8_t *val, int len)
{
    shadow_store(t, reg, val, len);
    int pos = 0;
    while (len > 0) {
        int size = len > MAX_I2C_MSG_LEN - 1 ? MAX_I2C_MSG_LEN - 1 : len;
        t->buf[0] = reg;
        memcpy(&t->buf[1], &val[pos], size);
        if (t->i2c_write(t->i2c_addr, t->buf, size + 1) != size + 1) {
            ESP_LOGE(TAG, "i2c write failed, reg 0x%02x", reg);
            return -1;
        }
        reg += size;
        len -= size;
        pos += size;
    }
    return 0;
}

static int write_reg(r82xx_t *t, uint8_t reg, uint8_t val)
{
    return write_regs(t, reg, &val, 1);
}

static int write_reg_mask(r82xx_t *t, uint8_t reg, uint8_t val, uint8_t mask)
{
    int r = reg - REG_SHADOW_START;
    if (r < 0 || r >= R82XX_NUM_REGS) return -1;
    val = (t->regs[r] & ~mask) | (val & mask);
    return write_regs(t, reg, &val, 1);
}

static uint8_t bitrev(uint8_t b)
{
    static const uint8_t lut[16] = {0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
                                    0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf};
    return (lut[b & 0xf] << 4) | lut[b >> 4];
}

// Reads always start at register 0; the chip sends each byte bit-reversed.
static int read_regs(r82xx_t *t, uint8_t *val, int len)
{
    t->buf[0] = 0x00;
    if (t->i2c_write(t->i2c_addr, t->buf, 1) != 1) return -1;
    if (t->i2c_read(t->i2c_addr, &t->buf[1], len) != len) {
        ESP_LOGE(TAG, "i2c read failed");
        return -1;
    }
    for (int i = 0; i < len; i++) val[i] = bitrev(t->buf[1 + i]);
    return 0;
}

// Shorthand: bail out of the calling function on the first failed write.
#define TRY(x)                 \
    do {                       \
        int rc_ = (x);         \
        if (rc_ < 0) return rc_; \
    } while (0)

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

static int set_mux(r82xx_t *t, uint32_t freq)
{
    uint32_t mhz = freq / 1000000;
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(k_ranges) - 1; i++) {
        if (mhz < k_ranges[i + 1].mhz) break;
    }
    TRY(write_reg_mask(t, 0x17, k_ranges[i].open_d, 0x08));    // open drain
    TRY(write_reg_mask(t, 0x1a, k_ranges[i].rf_mux, 0xc3));    // RF mux, polymux
    TRY(write_reg(t, 0x1b, k_ranges[i].tf_c));                 // tracking filter band
    TRY(write_reg_mask(t, 0x10, k_ranges[i].xtal_cap, 0x0b));  // xtal cap and drive
    TRY(write_reg_mask(t, 0x08, 0x00, 0x3f));
    return write_reg_mask(t, 0x09, 0x00, 0x3f);
}

static int set_pll(r82xx_t *t, uint32_t freq)
{
    const uint32_t vco_min = 1770000, vco_max = vco_min * 2;  // kHz
    const uint8_t vco_power_ref = 2;
    uint32_t freq_khz = (freq + 500) / 1000;
    uint32_t pll_ref = t->xtal_hz, pll_ref_khz = (t->xtal_hz + 500) / 1000;
    uint8_t mix_div = 2, div_num = 0, data[5];

    TRY(write_reg_mask(t, 0x10, 0x00, 0x10));  // refdiv2 off
    TRY(write_reg_mask(t, 0x1a, 0x00, 0x0c));  // PLL autotune 128 kHz
    TRY(write_reg_mask(t, 0x12, 0x06, 0xff));  // VCO current max (RTL-SDR Blog change)

    // Mixer divider that puts the VCO in its range.
    while (mix_div <= 64) {
        if (freq_khz * mix_div >= vco_min && freq_khz * mix_div < vco_max) {
            for (uint8_t d = mix_div; d > 2; d >>= 1) div_num++;
            break;
        }
        mix_div <<= 1;
    }
    TRY(read_regs(t, data, sizeof(data)));
    uint8_t vco_fine_tune = (data[4] & 0x30) >> 4;
    if (vco_fine_tune > vco_power_ref) div_num--;
    else if (vco_fine_tune < vco_power_ref) div_num++;
    TRY(write_reg_mask(t, 0x10, div_num << 5, 0xe0));

    uint64_t vco_freq = (uint64_t)freq * mix_div;
    uint32_t nint = vco_freq / (2 * pll_ref);
    uint32_t vco_fra = (vco_freq - 2ULL * pll_ref * nint) / 1000;  // kHz
    if (nint > 128 / vco_power_ref - 1) {
        ESP_LOGE(TAG, "no valid PLL values for %lu Hz", (unsigned long)freq);
        return -1;
    }
    if (nint < 13) {
        ESP_LOGE(TAG, "no valid PLL values for %lu Hz", (unsigned long)freq);
        return -1;
    }
    uint8_t ni = (nint - 13) / 4, si = nint - 4 * ni - 13;
    TRY(write_reg(t, 0x14, ni + (si << 6)));
    TRY(write_reg_mask(t, 0x12, vco_fra ? 0x00 : 0x08, 0x08));  // sigma-delta off if not needed

    uint16_t n_sdm = 2, sdm = 0;
    while (vco_fra > 1) {
        if (vco_fra > 2 * pll_ref_khz / n_sdm) {
            sdm += 32768 / (n_sdm / 2);
            vco_fra -= 2 * pll_ref_khz / n_sdm;
            if (n_sdm >= 0x8000) break;
        }
        n_sdm <<= 1;
    }
    TRY(write_reg(t, 0x16, sdm >> 8));
    TRY(write_reg(t, 0x15, sdm & 0xff));

    for (int i = 0; i < 2; i++) {
        TRY(read_regs(t, data, 3));
        if (data[2] & 0x40) break;
        if (!i) TRY(write_reg_mask(t, 0x12, 0x06, 0xff));  // not locked yet: max VCO current
    }
    t->has_lock = data[2] & 0x40;
    if (!t->has_lock) {
        ESP_LOGW(TAG, "PLL not locked at %lu Hz", (unsigned long)freq);
        return 0;
    }
    return write_reg_mask(t, 0x1a, 0x08, 0x08);  // PLL autotune 8 kHz
}

// librtlsdr's "DVB-T" set-up, which is what every SDR program uses.
static int sysfreq_sel(r82xx_t *t)
{
    const uint8_t mixer_top = 0x24, lna_top = 0xe5, lna_discharge = 14;
    TRY(write_reg_mask(t, 0x1d, lna_top, 0xc7));
    TRY(write_reg_mask(t, 0x1c, mixer_top, 0xf8));
    TRY(write_reg(t, 0x0d, 0x53));             // LNA vth 0.84, vtl 0.64
    TRY(write_reg(t, 0x0e, 0x75));             // mixer vth 1.04, vtl 0.84
    TRY(write_reg_mask(t, 0x05, 0x00, 0x60));  // air-in
    TRY(write_reg_mask(t, 0x06, 0x00, 0x08));
    TRY(write_reg_mask(t, 0x11, 0x38, 0x38));  // charge pump current auto
    TRY(write_reg_mask(t, 0x17, 0xa0, 0x30));  // PLL drop-out 2.0 V (RTL-SDR Blog change)
    TRY(write_reg_mask(t, 0x0a, 0x40, 0x60));  // filter current low

    TRY(write_reg_mask(t, 0x1d, 0x00, 0x38));  // LNA top lowest
    TRY(write_reg_mask(t, 0x1c, 0x00, 0x04));  // normal mode
    TRY(write_reg_mask(t, 0x06, 0x00, 0x40));  // pre-detect off
    TRY(write_reg_mask(t, 0x1a, 0x30, 0x30));  // AGC clock 250 Hz
    TRY(write_reg_mask(t, 0x1d, 0x18, 0x38));  // LNA top 3
    TRY(write_reg_mask(t, 0x1c, mixer_top, 0x04));
    TRY(write_reg_mask(t, 0x1e, lna_discharge, 0x1f));
    return write_reg_mask(t, 0x1a, 0x20, 0x30);  // AGC clock 60 Hz
}

// Calibrates the IF filter and sets the 6 MHz defaults; r82xx_set_bandwidth
// narrows it afterwards.
static int set_tv_standard(r82xx_t *t)
{
    const uint8_t hp_cor = 0x6b;  // 1.7 MHz disable, +2 cap, 1.0 MHz
    uint8_t data[5];

    memcpy(t->regs, k_init_array, sizeof(k_init_array));
    TRY(write_reg_mask(t, 0x0c, 0x00, 0x0f));
    TRY(write_reg_mask(t, 0x13, VER_NUM, 0x3f));
    TRY(write_reg_mask(t, 0x1d, 0x00, 0x38));
    t->int_freq = 3570000;

    for (int i = 0; i < 2; i++) {
        TRY(write_reg_mask(t, 0x0b, hp_cor, 0x60));
        TRY(write_reg_mask(t, 0x0f, 0x04, 0x04));  // calibration clock on
        TRY(write_reg_mask(t, 0x10, 0x00, 0x03));  // xtal cap 0 pF for the PLL
        TRY(set_pll(t, 56000000));
        if (!t->has_lock) return -1;
        TRY(write_reg_mask(t, 0x0b, 0x10, 0x10));  // start trigger
        TRY(write_reg_mask(t, 0x0b, 0x00, 0x10));  // stop trigger
        TRY(write_reg_mask(t, 0x0f, 0x00, 0x04));  // calibration clock off
        TRY(read_regs(t, data, sizeof(data)));
        t->fil_cal_code = data[4] & 0x0f;
        if (t->fil_cal_code && t->fil_cal_code != 0x0f) break;
    }
    if (t->fil_cal_code == 0x0f) t->fil_cal_code = 0;  // narrowest

    TRY(write_reg_mask(t, 0x0a, 0x10 | t->fil_cal_code, 0x1f));  // low Q
    TRY(write_reg_mask(t, 0x0b, hp_cor, 0xef));
    TRY(write_reg_mask(t, 0x07, 0x00, 0x80));  // image negative
    TRY(write_reg_mask(t, 0x06, 0x30, 0x30));  // filter +3 dB, 6 MHz on
    TRY(write_reg_mask(t, 0x1e, 0x60, 0x60));  // channel filter extension
    TRY(write_reg_mask(t, 0x05, 0x80, 0x80));  // loop-through off
    TRY(write_reg_mask(t, 0x1f, 0x00, 0x80));  // loop-through attenuation
    TRY(write_reg_mask(t, 0x0f, 0x00, 0x80));  // filter extension widest off
    return write_reg_mask(t, 0x19, 0x60, 0x60);  // RF poly filter current min
}

int r82xx_init(r82xx_t *t)
{
    TRY(write_regs(t, 0x05, k_init_array, sizeof(k_init_array)));
    TRY(set_tv_standard(t));
    return sysfreq_sel(t);
}

// Bandwidth contributed by each IF low-pass filter setting.
static const int k_lpf_bw[] = {1700000, 1600000, 1550000, 1450000, 1200000, 900000, 700000, 550000, 450000, 350000};
#define FILT_HP_BW1 350000
#define FILT_HP_BW2 380000

int r82xx_set_bandwidth(r82xx_t *t, uint32_t bw_hz)
{
    int bw = bw_hz, real_bw = 0;
    uint8_t reg_0a, reg_0b;
    if (bw > 7000000) {
        reg_0a = 0x10, reg_0b = 0x0b, t->int_freq = 4570000;
    } else if (bw > 6000000) {
        reg_0a = 0x10, reg_0b = 0x2a, t->int_freq = 4570000;
    } else if (bw > k_lpf_bw[0] + FILT_HP_BW1 + FILT_HP_BW2) {
        reg_0a = 0x10, reg_0b = 0x6b, t->int_freq = 3570000;
    } else {
        reg_0a = 0x00, reg_0b = 0x80, t->int_freq = 2300000;
        if (bw > k_lpf_bw[0] + FILT_HP_BW1) {
            bw -= FILT_HP_BW2;
            t->int_freq += FILT_HP_BW2;
            real_bw += FILT_HP_BW2;
        } else {
            reg_0b |= 0x20;
        }
        if (bw > k_lpf_bw[0]) {
            bw -= FILT_HP_BW1;
            t->int_freq += FILT_HP_BW1;
            real_bw += FILT_HP_BW1;
        } else {
            reg_0b |= 0x40;
        }
        unsigned i;
        for (i = 0; i < ARRAY_SIZE(k_lpf_bw); i++) {
            if (bw > k_lpf_bw[i]) break;
        }
        i--;
        reg_0b |= 15 - i;
        real_bw += k_lpf_bw[i];
        t->int_freq -= real_bw / 2;
    }
    TRY(write_reg_mask(t, 0x0a, reg_0a, 0x10));
    TRY(write_reg_mask(t, 0x0b, reg_0b, 0xef));
    return t->int_freq;
}

int r82xx_set_freq(r82xx_t *t, uint32_t freq_hz)
{
    uint32_t lo = freq_hz + t->int_freq;
    TRY(set_mux(t, lo));
    TRY(write_reg_mask(t, 0x0c, 0x08, 0x9f));  // fixed VGA gain, 16.3 dB
    TRY(set_pll(t, lo));
    return t->has_lock ? 0 : -1;
}

// Gain steps in tenths of a dB, measured by Steve Markgraf.
static const int k_lna_gain_steps[] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
static const int k_mixer_gain_steps[] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};

int r82xx_set_gain(r82xx_t *t, bool manual, int tenth_db)
{
    if (!manual) {
        TRY(write_reg_mask(t, 0x05, 0x00, 0x10));  // LNA auto
        TRY(write_reg_mask(t, 0x07, 0x10, 0x10));  // mixer auto
        return write_reg_mask(t, 0x0c, 0x0b, 0x9f);  // fixed VGA gain, 26.5 dB
    }
    uint8_t data[4];
    TRY(write_reg_mask(t, 0x05, 0x10, 0x10));  // LNA auto off
    TRY(write_reg_mask(t, 0x07, 0x00, 0x10));  // mixer auto off
    TRY(read_regs(t, data, sizeof(data)));
    TRY(write_reg_mask(t, 0x0c, 0x08, 0x9f));  // fixed VGA gain, 16.3 dB

    int total = 0;
    uint8_t lna = 0, mix = 0;
    for (int i = 0; i < 15; i++) {
        if (total >= tenth_db) break;
        total += k_lna_gain_steps[++lna];
        if (total >= tenth_db) break;
        total += k_mixer_gain_steps[++mix];
    }
    TRY(write_reg_mask(t, 0x05, lna, 0x0f));
    return write_reg_mask(t, 0x07, mix, 0x0f);
}
