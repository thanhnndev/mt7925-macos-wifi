// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../kexts/itlmt7925/itl_power.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <fstream>
#include <iterator>
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
int main(int argc, char **argv)
{
    int8_t sar[11]; int dv, gv;
    uint8_t dyn[19] = {}, geo[28] = {};
    dyn[4] = 1; dyn[6] = 1; memset(dyn + 8, 40, 11);
    geo[4] = 1; geo[6] = 3; // a v2 layout with a v1 version byte
    geo[22] = 50; geo[23] = 200; // unsigned offset must saturate rather than wrap
    geo[24] = 0xff; geo[25] = 50; // absent geo limit ignores its offset
    geo[26] = 30;
    parseSar(dyn, sizeof(dyn), geo, sizeof(geo), sar, &dv, &gv);
    assert(dv == 2 && gv == 2 && sar[0] == 50 && sar[1] == 40 && sar[5] == 30);
    dyn[8] = 0xff; geo[22] = 0xff;
    parseSar(dyn, sizeof(dyn), geo, sizeof(geo), sar, &dv, &gv);
    assert(sar[0] == 127);
    uint8_t v1[12] = {}; v1[4] = 1; v1[5] = 1; memset(v1 + 7, 32, 5);
    parseSar(v1, sizeof(v1), nullptr, -1, sar, &dv, &gv);
    assert(dv == 1 && gv == 0 && sar[4] == 32 && sar[5] == 127);
    assert(sarFor(sar, 2437) == 32 && sarFor(sar, 5350) == 32 && sarFor(sar, 7125) == 127);
    parseSar(dyn, 18, geo, sizeof(geo), sar, &dv, &gv);
    assert(dv == 0 && sar[0] == 127);
    dyn[4] = 0;
    parseSar(dyn, sizeof(dyn), geo, sizeof(geo), sar, &dv, &gv);
    assert(dv == 0 && sar[5] == 127);
    int8_t guarded[SKU_LEN + 2]; guarded[0] = 99; guarded[SKU_LEN + 1] = 99;
    buildSku(guarded + 1, 40, true);
    const int8_t *sku = guarded + 1;
    for (unsigned i = 0; i < SKU_LEN; i++) {
        bool used = i < 12 || (i >= 44 && i < 61) || (i >= 109);
        if (i >= 61 && i < 109) used = (i - 61) % 12 < 10;
        assert(sku[i] == (used ? 40 : 127));
    }
    assert(guarded[0] == 99 && guarded[SKU_LEN + 1] == 99);
    buildSku(guarded + 1, 25, false); assert(sku[0] == 127 && sku[4] == 25 && sku[448] == 25);
    uint8_t c[64] = {}; c[8] = 1; wr32(c + 20, 24); wr32(c + 24, 8);
    uint32_t begin, end, off, size;
    assert(clcRuleBounds(c, sizeof(c), &begin, &end) && begin == 16 && end == 24);
    assert(clcSegmentBounds(c, sizeof(c), 1, &off, &size));
    assert(!clcSegmentBounds(c, sizeof(c), 0, &off, &size));
    wr32(c + 20, 0xffffffff); assert(!clcRuleBounds(c, sizeof(c), &begin, &end));
    assert(!clcSegmentBounds(c, sizeof(c), 1, &off, &size));
    assert(!clcRuleBounds(c, 15, &begin, &end));
    // Walk the actual firmware, checking every selected POWER/POWER_EXT world rule with production bounds checks.
    assert(argc == 2);
    std::ifstream f(argv[1], std::ios::binary); assert(f);
    std::vector<uint8_t> fw((std::istreambuf_iterator<char>(f)), {});
    assert(fw.size() >= 36); unsigned nr = fw[fw.size() - 34];
    size_t payload = fw.size() - 36 - nr * 40, offset = 0; unsigned worlds = 0, tables = 0;
    for (unsigned i = 0; i < nr; i++) {
        const uint8_t *r = fw.data() + payload + i * 40; uint32_t len = rd32(r + 20);
        assert(len <= payload - offset);
        if ((r[24] & 64) && r[25] == 2) {
            for (uint32_t o = 0; o + 16 <= len;) {
                const uint8_t *t = fw.data() + offset + o; uint32_t tl = rd32(t);
                assert(tl >= 16 && tl <= len - o);
                if (t[4] < 2) {
                    tables++; assert(clcRuleBounds(t, tl, &begin, &end));
                    for (unsigned j = begin; j < end; j += 8) {
                        const uint8_t *rule = t + 16 + j;
                        assert(clcSegmentBounds(t, tl, rule[4], &off, &size));
                        if (rule[0] == '0' && rule[1] == '0') worlds++;
                    }
                }
                o += tl;
            }
        }
        offset += len;
    }
    assert(tables && worlds);
    printf("power tests passed; firmware: %u power tables, %u world rules\n", tables, worlds);
}
