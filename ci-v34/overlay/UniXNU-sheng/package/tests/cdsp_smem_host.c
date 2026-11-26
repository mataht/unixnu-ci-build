#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../QcomSmemGlobal.h"

static uint8_t gBuf[0x10000];

static void wr32(uint32_t off, uint32_t v)
{
    memcpy(gBuf + off, &v, 4);
}

int main(void)
{
    memset(gBuf, 0, sizeof(gBuf));
    const uint32_t total = sizeof(gBuf);
    const uint32_t toc = total - 0x1000u;

    // Build $TOC with 2 partitions: (0, 5) at 0x4000..0x6000 and (0xfffe, 0xfffe) at 0x6000..0x8000
    wr32(toc + 0, 0x434f5424u); // "$TOC"
    wr32(toc + 4, 1u);
    wr32(toc + 8, 2u);

    // Entry 0: private (0, 5)
    wr32(toc + 32 + 0, 0x4000u);
    wr32(toc + 32 + 4, 0x2000u);
    wr32(toc + 32 + 12, 0u | (5u << 16));
    wr32(toc + 32 + 16, 32u);
    wr32(0x4000u + 0, 0x54525024u); // "$PRT"
    wr32(0x4000u + 4, 0u | (5u << 16));
    wr32(0x4000u + 8, 0x2000u);
    wr32(0x4000u + 12, 32u);
    wr32(0x4000u + 16, 0x2000u);

    // Entry 1: global (0xfffe, 0xfffe)
    wr32(toc + 32 + 48 + 0, 0x6000u);
    wr32(toc + 32 + 48 + 4, 0x2000u);
    wr32(toc + 32 + 48 + 12, 0xfffeu | (0xfffeu << 16));
    wr32(toc + 32 + 48 + 16, 32u);
    wr32(0x6000u + 0, 0x54525024u); // "$PRT"
    wr32(0x6000u + 4, 0xfffeu | (0xfffeu << 16));
    wr32(0x6000u + 8, 0x2000u);
    wr32(0x6000u + 12, 32u);
    wr32(0x6000u + 16, 0x2000u);

    QcomSmem smem = {gBuf, total};
    QcomSmemPartition priv = {}, glob = {};
    assert(qcom_smem_partition(&smem, 0u, 5u, &priv) == QcomSmemOk);
    assert(qcom_smem_partition(&smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &glob) == QcomSmemOk);

    // 1. Item 432 in private partition -> QcomSmemLocPrivate
    assert(qcom_smem_alloc(&smem, &priv, 432u, 64u) == QcomSmemOk);
    uint32_t off = 0, sz = 0;
    assert(qcom_smem_find_any(&smem, 0x81d00000u, &priv, true, &glob, true, 432u, &off, &sz) == QcomSmemLocPrivate);
    assert(sz == 64u && off >= 0x4020u && off < 0x6000u);

    // 2. Legacy global header toc[94] -> QcomSmemLocLegacy when not in priv or glob
    wr32(QCOM_SMEM_HEADER_INITIALIZED_OFF, 1u);
    const uint32_t e94 = QCOM_SMEM_HEADER_TOC_OFF + 94u * 16u;
    wr32(e94 + 0, 1u);          // allocated
    wr32(e94 + 4, 0x3200u);     // offset
    wr32(e94 + 8, 340u);        // size
    wr32(e94 + 12, 0x81d00001u);// aux_base with flag bits in low 2 bits
    assert(qcom_smem_find_any(&smem, 0x81d00000u, &priv, true, &glob, true, 94u, &off, &sz) == QcomSmemLocLegacy);
    assert(off == 0x3200u && sz == 340u);

    // 3. Global $PRT item 94 takes precedence over legacy table -> QcomSmemLocGlobal
    assert(qcom_smem_alloc(&smem, &glob, 94u, 340u) == QcomSmemOk);
    assert(qcom_smem_find_any(&smem, 0x81d00000u, &priv, true, &glob, true, 94u, &off, &sz) == QcomSmemLocGlobal);
    assert(sz == 340u && off >= 0x6020u && off < 0x8000u);

    printf("cdsp_smem_host: all checks passed\n");
    return 0;
}
