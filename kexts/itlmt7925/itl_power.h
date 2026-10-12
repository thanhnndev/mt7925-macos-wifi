// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
#ifndef ITL_POWER_H
#define ITL_POWER_H
#include <stdint.h>
#include <string.h>
static const unsigned SKU_LEN = 449;
static const uint16_t sarRange[11][2] = {
    {2402, 2494}, {5150, 5350}, {5350, 5470}, {5470, 5725}, {5725, 5950},
    {5945, 6165}, {6165, 6405}, {6405, 6525}, {6525, 6705}, {6705, 6865}, {6865, 7125},
};

static inline bool asarFits(const uint8_t *t, int len, int pre, int unit, int nrOff, int min, int max)
{
    if (len < pre || (len - pre) % unit) return false;
    int cnt = (len - pre) / unit;
    return cnt >= min && cnt <= max && t[nrOff] == cnt;
}

// First dynamic profile and world geo row, matching mt792x_asar_range_pwr().
static inline void parseSar(const uint8_t *dyn, int dl, const uint8_t *geo, int gl,
                            int8_t sar[11], int *dynVer, int *geoVer)
{
    for (int i = 0; i < 11; i++) sar[i] = 127;
    const uint8_t *frp = nullptr;
    int nfrp = 0;
    if (dl > 0 && asarFits(dyn, dl, 6, 6, 5, 1, 8)) { frp = dyn + 6 + 1; nfrp = 5; }
    else if (dl > 0 && asarFits(dyn, dl, 7, 12, 6, 1, 8)) { frp = dyn + 7 + 1; nfrp = 11; }
    if (!frp || !dyn[4]) {                                   // no table, or dynamic SAR disabled
        *dynVer = *geoVer = 0;
        return;
    }
    const uint8_t *band = nullptr;                           // { pwr, offset } per band of geo row 2 (WW)
    int nband = 0;
    if (gl > 0 && asarFits(geo, gl, 6, 5, 5, 3, 8)) { band = geo + 6 + 2 * 5 + 1; nband = 2; }
    else if (gl > 0 && asarFits(geo, gl, 7, 7, 6, 3, 8)) { band = geo + 7 + 2 * 7 + 1; nband = 3; }
    for (int i = 0; i < 11 && i < nfrp; i++) {
        int p = frp[i] == 0xff ? 127 : frp[i];
        int b = sarRange[i][0] >= 5945 ? 2 : sarRange[i][0] >= 5150 ? 1 : 0;
        if (band && b < nband && band[2 * b] != 0xff) {
            int g = band[2 * b], o = band[2 * b + 1];
            p = p + o < 127 ? p + o : 127;
            if (g < p) p = g;
        }
        sar[i] = (int8_t)(p > 127 ? 127 : p);
    }
    *dynVer = nfrp == 11 ? 2 : 1;
    *geoVer = nband == 3 ? 2 : nband == 2 ? 1 : 0;
}

static inline void buildSku(int8_t *sku, int8_t v, bool g2)
{
    memset(sku, 127, SKU_LEN);
    int o = 0;
    if (g2) memset(sku, v, 4);                               // cck
    o = 4;
    memset(sku + o, v, 8); o += 8 * 5;                       // ofdm
    memset(sku + o, v, 16); o += 16;                         // ht: two groups of 8
    sku[o++] = v;                                            // ht40 mcs32
    for (int i = 0; i < 4; i++) { memset(sku + o, v, 10); o += 12; }   // vht 20/40/80/160
    memset(sku + o, v, 7 * 12); o += 7 * 12;                 // he ru
    memset(sku + o, v, 16 * 16);                             // eht
}

static inline int8_t sarFor(const int8_t sar[11], uint32_t freq)
{
    for (int i = 0; i < 11; i++)
        if (freq >= sarRange[i][0] && freq < sarRange[i][1]) return sar[i];
    return 127;
}


// All offsets are relative to the 16-byte CLC header's data area.
static inline bool clcRuleBounds(const uint8_t *clc, uint32_t len, uint32_t *begin, uint32_t *end)
{
    if (len < 16 || !clc[8]) return false;
    *begin = (uint32_t)clc[8] * 16;
    if (*begin > len - 16 || len - 16 - *begin < 8) return false;
    memcpy(end, clc + 20, 4);
    return *end >= *begin && *end <= len - 16 && (*end - *begin) % 8 == 0;
}
static inline bool clcSegmentBounds(const uint8_t *clc, uint32_t len, uint8_t idx,
                                    uint32_t *offset, uint32_t *size)
{
    if (len < 16 || !idx || idx > clc[8] || (uint32_t)clc[8] * 16 > len - 16) return false;
    const uint8_t *seg = clc + 16 + ((uint32_t)idx - 1) * 16;
    memcpy(offset, seg + 4, 4); memcpy(size, seg + 8, 4);
    return *size && *size <= 2048 && *offset <= len - 16 && *size <= len - 16 - *offset;
}
#endif
