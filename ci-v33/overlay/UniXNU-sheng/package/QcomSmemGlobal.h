// SPDX-License-Identifier: BSD-2-Clause-Patent
// Multi-tier Qualcomm SMEM lookup helper matching Linux drivers/soc/qcom/smem.c (qcom_smem_get):
//   1. Host-pair private $PRT partition (e.g., APPS=0 <-> CDSP=5 for items >= 400: 432, 478, 479, 480)
//   2. Global $PRT partition (QCOM_SMEM_HOST_GLOBAL 0xfffe <-> 0xfffe for non-partitioned items < 400 such as CDSP SMP2P 94)
//   3. Legacy global smem_header.toc[512] at SMEM base + 0xd0 (struct smem_header: initialized==1 at +0xc0, 16-byte entries)
// Shared between CdspProbe.cpp (kernel) and package/tests/cdsp_smem_host.c (host unit test).
#ifndef QCOM_SMEM_GLOBAL_H
#define QCOM_SMEM_GLOBAL_H

#include <stdint.h>
#include <stdbool.h>
#include "QcomSmem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_SMEM_HEADER_INITIALIZED_OFF 0xc0u
#define QCOM_SMEM_HEADER_TOC_OFF         0xd0u
#define QCOM_SMEM_HEADER_TOC_COUNT       512u
#define QCOM_SMEM_HEADER_TOC_STRIDE      16u
#define QCOM_SMEM_AUX_BASE_MASK          0xfffffffcu

enum {
    QcomSmemLocNone    = 0,
    QcomSmemLocPrivate = 1,
    QcomSmemLocGlobal  = 2,
    QcomSmemLocLegacy  = 3,
};

static inline int qcom_smem_find_any(
    const QcomSmem *smem,
    uint32_t phys_base,
    const QcomSmemPartition *priv,
    bool has_priv,
    const QcomSmemPartition *glob,
    bool has_glob,
    uint16_t item,
    uint32_t *offset_out,
    uint32_t *size_out)
{
    uint32_t at = 0, bytes = 0;
    if (!smem || !smem->base || smem->size < 0x3000u) return QcomSmemLocNone;

    if (has_priv && priv && qcom_smem_find(smem, priv, item, &at, &bytes) == QcomSmemOk) {
        if (offset_out) *offset_out = at;
        if (size_out) *size_out = bytes;
        return QcomSmemLocPrivate;
    }

    if (has_glob && glob && qcom_smem_find(smem, glob, item, &at, &bytes) == QcomSmemOk) {
        if (offset_out) *offset_out = at;
        if (size_out) *size_out = bytes;
        return QcomSmemLocGlobal;
    }

    if (item < QCOM_SMEM_HEADER_TOC_COUNT && qcom_smem_read32(smem, QCOM_SMEM_HEADER_INITIALIZED_OFF) == 1u) {
        const uint32_t e = QCOM_SMEM_HEADER_TOC_OFF + (uint32_t)item * QCOM_SMEM_HEADER_TOC_STRIDE;
        const uint32_t allocated = qcom_smem_read32(smem, e);
        const uint32_t off       = qcom_smem_read32(smem, e + 4u);
        const uint32_t sz        = qcom_smem_read32(smem, e + 8u);
        const uint32_t aux       = qcom_smem_read32(smem, e + 12u) & QCOM_SMEM_AUX_BASE_MASK;
        const uint32_t limit     = smem->size - 0x1000u;
        if (allocated != 0u && sz > 0u && (aux == 0u || aux == phys_base) &&
            off <= limit && sz <= limit - off) {
            if (offset_out) *offset_out = off;
            if (size_out) *size_out = sz;
            return QcomSmemLocLegacy;
        }
    }

    return QcomSmemLocNone;
}

#ifdef __cplusplus
}
#endif

#endif // QCOM_SMEM_GLOBAL_H
