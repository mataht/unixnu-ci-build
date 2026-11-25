// SPDX-License-Identifier: BSD-2-Clause-Patent
// Virtual AppleSMC key table, wire layout, and SM8550 power/energy estimation model.
// Verified against macOS 27 arm64e libSMC.dylib (SMCKextCall 168-byte SMCParamStruct) and legacy 80-byte IOKit SMCKeyData_t.
// Shared between ShengSMC.cpp (kernel) and package/tests/smc_keys_host.c (host unit test).
#ifndef SHENG_SMC_KEYS_H
#define SHENG_SMC_KEYS_H

#include <stdint.h>
#include <stdbool.h>
#ifndef KERNEL
#include <string.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SHENG_SMC_FOURCC(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8)  | ((uint32_t)(uint8_t)(d)))

// External method selectors on AppleSMCClient (verified in libSMC.dylib SMCOpenAppleSMC / SMCCloseAppleSMC / SMCKextCall)
#define SHENG_SMC_SEL_OPEN         0u
#define SHENG_SMC_SEL_CLOSE        1u
#define SHENG_SMC_SEL_KEXT_CALL    2u

// Sub-commands in SMCParamStruct.data8 (offset 0x2a)
#define SHENG_SMC_CMD_READ_KEY     5u
#define SHENG_SMC_CMD_WRITE_KEY    6u
#define SHENG_SMC_CMD_GET_KEY_IDX  8u
#define SHENG_SMC_CMD_GET_KEY_INFO 9u

// Result codes in SMCParamStruct.result (offset 0x28)
#define SHENG_SMC_RESULT_OK             0x00u
#define SHENG_SMC_RESULT_KEY_NOT_FOUND  0x84u
#define SHENG_SMC_RESULT_BAD_INDEX      0xb8u  // verified at SMCGetKeyFromIndex+192 (cmp w8, #0xb8)

// Wire sizes: header is 0x30 (48) bytes; legacy IOKit passes 80 (0x30+32), Apple Silicon libSMC.dylib passes 168 (0x30+120).
#define SHENG_SMC_HEADER_BYTES     48u
#define SHENG_SMC_LEGACY_BYTES     80u
#define SHENG_SMC_MODERN_BYTES     168u

// SMC data types (FourCC)
#define SHENG_SMC_TYPE_UI8         SHENG_SMC_FOURCC('u', 'i', '8', ' ')
#define SHENG_SMC_TYPE_UI16        SHENG_SMC_FOURCC('u', 'i', '1', '6')
#define SHENG_SMC_TYPE_UI32        SHENG_SMC_FOURCC('u', 'i', '3', '2')
#define SHENG_SMC_TYPE_FLT         SHENG_SMC_FOURCC('f', 'l', 't', ' ')
#define SHENG_SMC_TYPE_CH8         SHENG_SMC_FOURCC('c', 'h', '8', '*')
#define SHENG_SMC_TYPE_REV         SHENG_SMC_FOURCC('{', 'r', 'e', 'v')
#define SHENG_SMC_ATTR_READABLE    0x80u

// Exact 168-byte Apple Silicon SMCParamStruct layout (offsets verified from libSMC.dylib disassembly):
//   0x00: key (u32)
//   0x04..0x1b: vers (6B) + pad (2B) + pLimitData (16B)
//   0x1c: keyInfo.dataSize (u32)
//   0x20: keyInfo.dataType (u32)
//   0x24: keyInfo.dataAttributes (u8)
//   0x28: result (u8), 0x29: status (u8), 0x2a: data8 (u8)
//   0x2c: data32 (u32)
//   0x30..0xa7: bytes[120]
struct ShengSmcParam {
    uint32_t key;
    uint8_t  vers[6];
    uint8_t  versPad[2];
    uint8_t  pLimitData[16];
    uint32_t dataSize;
    uint32_t dataType;
    uint8_t  dataAttributes;
    uint8_t  infoPad[3];
    uint8_t  result;
    uint8_t  status;
    uint8_t  data8;
    uint8_t  cmdPad;
    uint32_t data32;
    uint8_t  bytes[120];
};

#ifdef __cplusplus
static_assert(sizeof(struct ShengSmcParam) == SHENG_SMC_MODERN_BYTES, "SMCParamStruct 168 bytes");
static_assert(__builtin_offsetof(struct ShengSmcParam, dataSize) == 0x1c, "dataSize offset");
static_assert(__builtin_offsetof(struct ShengSmcParam, dataType) == 0x20, "dataType offset");
static_assert(__builtin_offsetof(struct ShengSmcParam, dataAttributes) == 0x24, "dataAttributes offset");
static_assert(__builtin_offsetof(struct ShengSmcParam, result) == 0x28, "result offset");
static_assert(__builtin_offsetof(struct ShengSmcParam, data8) == 0x2a, "data8 offset");
static_assert(__builtin_offsetof(struct ShengSmcParam, data32) == 0x2c, "data32 offset");
static_assert(__builtin_offsetof(struct ShengSmcParam, bytes) == 0x30, "bytes offset");
#endif

struct ShengSmcSnapshot {
    float cpu_max_c;
    float cpu_e_c;
    float cpu_p_c;
    float cpu_x3_c;
    float gpu_c;
    float soc_c;
    float wifi_c;
    float npu_c;
    float batt_c;

    float sys_w;
    float cpu_w;
    float ecpu_w;
    float pcpu_w;
    float gpu_w;
    float npu_w;
    float dram_w;
    float batt_w;
    float batt_v;
    float batt_a;

    uint8_t thermal_level;
    uint8_t external_connected;
    uint8_t battery_present;
};

// Estimate instantaneous SM8550 domain milliwatts from live frequencies/busy and 1S2P battery/USB telemetry.
static inline void sheng_smc_estimate_power(
    const uint32_t mhz[3],
    const uint32_t busy_pct[3],
    uint32_t gpu_level,
    uint32_t gpu_busy_pct,
    bool cdsp_active,
    int32_t batt_mv,
    int32_t batt_ma,
    bool usb_external,
    struct ShengSmcSnapshot *s)
{
    // E-cluster (3x A510, 307..2016 MHz): ~80 mW idle leakage + up to ~650 mW dynamic
    const float fe = mhz[0] ? (float)mhz[0] / 2016.0f : 0.25f;
    const float be = (busy_pct[0] <= 100u ? (float)busy_pct[0] : 100.0f) * 0.01f;
    s->ecpu_w = 0.08f + 0.65f * (fe * fe) * be;

    // P-cluster (4x A715/A710, 499..2803 MHz) + X3 (cpu7, 595..3187 MHz)
    const float fp = mhz[1] ? (float)mhz[1] / 2803.0f : 0.25f;
    const float bp = (busy_pct[1] <= 100u ? (float)busy_pct[1] : 100.0f) * 0.01f;
    const float fx = mhz[2] ? (float)mhz[2] / 3187.0f : 0.25f;
    const float bx = (busy_pct[2] <= 100u ? (float)busy_pct[2] : 100.0f) * 0.01f;
    s->pcpu_w = 0.22f + 2.85f * (fp * fp * fp) * bp + 1.55f * (fx * fx * fx) * bx;
    s->cpu_w  = s->ecpu_w + s->pcpu_w;

    // Adreno 740 GPU (L1=220 MHz .. L8=680 MHz)
    static const uint16_t kGpuMhz[9] = {220, 220, 295, 348, 401, 475, 525, 578, 680};
    const unsigned gl = gpu_level <= 8u ? gpu_level : 8u;
    const float fg = (float)kGpuMhz[gl] / 680.0f;
    const float bg = (gpu_busy_pct <= 100u ? (float)gpu_busy_pct : 100.0f) * 0.01f;
    s->gpu_w  = 0.18f + 3.60f * (fg * fg) * bg;

    // Hexagon HTP V73 CDSP/NSP + LPDDR5X / uncore
    s->npu_w  = cdsp_active ? 0.06f : 0.0f;
    s->dram_w = 0.28f + 0.35f * (be * 0.2f + bp * 0.5f + bg * 0.3f);

    const float modeled_board_w = s->cpu_w + s->gpu_w + s->npu_w + s->dram_w + 1.45f; // + panel/backlight/Wi-Fi base
    if (batt_mv > 2500 && batt_mv < 10000) {
        s->batt_v = (float)batt_mv * 0.001f;
        s->batt_a = (float)batt_ma * 0.001f;
        const float net_batt_w = s->batt_v * (-s->batt_a); // positive when discharging
        const float usb_in_w = usb_external ? 2.15f : 0.0f;
        const float measured_w = net_batt_w + usb_in_w;
        s->batt_w = net_batt_w >= 0.0f ? net_batt_w : -net_batt_w;
        s->sys_w  = measured_w > 0.8f ? measured_w : modeled_board_w;
    } else {
        s->batt_v = 4.20f;
        s->batt_a = 0.0f;
        s->batt_w = modeled_board_w;
        s->sys_w  = modeled_board_w;
    }
}

struct ShengSmcKeyDef {
    uint32_t key;
    uint32_t type;
    uint8_t  size;
};

static const struct ShengSmcKeyDef kShengSmcKeys[] = {
    {SHENG_SMC_FOURCC('#', 'K', 'E', 'Y'), SHENG_SMC_TYPE_UI32, 4},
    {SHENG_SMC_FOURCC('$', 'A', 'd', 'r'), SHENG_SMC_TYPE_UI32, 4},
    {SHENG_SMC_FOURCC('$', 'N', 'u', 'm'), SHENG_SMC_TYPE_UI8,  1},
    {SHENG_SMC_FOURCC('R', 'E', 'V', ' '), SHENG_SMC_TYPE_REV,  6},
    {SHENG_SMC_FOURCC('R', 'P', 'l', 't'), SHENG_SMC_TYPE_CH8,  8},
    {SHENG_SMC_FOURCC('O', 'S', 'K', '0'), SHENG_SMC_TYPE_CH8,  32},
    {SHENG_SMC_FOURCC('O', 'S', 'K', '1'), SHENG_SMC_TYPE_CH8,  32},
    {SHENG_SMC_FOURCC('F', 'N', 'u', 'm'), SHENG_SMC_TYPE_UI8,  1},
    {SHENG_SMC_FOURCC('m', 'T', 'P', 'L'), SHENG_SMC_TYPE_UI8,  1},
    {SHENG_SMC_FOURCC('A', 'C', '-', 'N'), SHENG_SMC_TYPE_UI8,  1},
    {SHENG_SMC_FOURCC('B', 'A', 'T', 'P'), SHENG_SMC_TYPE_UI8,  1},
    // Temperatures ('flt ', 4-byte IEEE-754 little-endian float on Apple Silicon libSMC.dylib)
    {SHENG_SMC_FOURCC('T', 'C', '0', 'P'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'C', '0', 'D'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'C', '0', 'E'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'C', '0', 'F'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'C', '0', 'G'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'p', '0', '1'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'p', '0', '5'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'p', '0', '9'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'p', '0', 'D'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'G', '0', 'P'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'G', '0', 'D'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 's', '0', 'P'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'W', '0', 'P'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'B', '0', 'T'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'B', '1', 'T'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('T', 'N', '0', 'P'), SHENG_SMC_TYPE_FLT,  4},
    // Power, Voltage & Current ('flt ')
    {SHENG_SMC_FOURCC('P', 'S', 'T', 'R'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('P', 'C', 'P', 'C'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('P', 'G', '0', 'R'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('P', 'D', 'T', 'R'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('V', 'D', '0', 'R'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('V', 'B', '0', 'R'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('I', 'D', '0', 'R'), SHENG_SMC_TYPE_FLT,  4},
    {SHENG_SMC_FOURCC('I', 'B', '0', 'R'), SHENG_SMC_TYPE_FLT,  4},
};

#define SHENG_SMC_KEY_COUNT ((uint32_t)(sizeof(kShengSmcKeys) / sizeof(kShengSmcKeys[0])))

static inline void sheng_smc_put_be32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)v;
}

static inline void sheng_smc_put_flt_le(uint8_t *dst, float v)
{
    memcpy(dst, &v, sizeof(float));
}

static inline const struct ShengSmcKeyDef *sheng_smc_find_key(uint32_t key)
{
    for (uint32_t i = 0; i < SHENG_SMC_KEY_COUNT; ++i) {
        if (kShengSmcKeys[i].key == key) return &kShengSmcKeys[i];
    }
    return NULL;
}

static inline void sheng_smc_fill_value(uint32_t key, const struct ShengSmcSnapshot *s, uint8_t out_bytes[32])
{
    memset(out_bytes, 0, 32);
    switch (key) {
    case SHENG_SMC_FOURCC('#', 'K', 'E', 'Y'): sheng_smc_put_be32(out_bytes, SHENG_SMC_KEY_COUNT); break;
    case SHENG_SMC_FOURCC('$', 'A', 'd', 'r'): sheng_smc_put_be32(out_bytes, 0x00000300u); break;
    case SHENG_SMC_FOURCC('$', 'N', 'u', 'm'): out_bytes[0] = 1u; break;
    case SHENG_SMC_FOURCC('R', 'E', 'V', ' '):
        out_bytes[0] = 2; out_bytes[1] = 45; out_bytes[2] = 0x0f; out_bytes[3] = 0; out_bytes[4] = 0; out_bytes[5] = 1;
        break;
    case SHENG_SMC_FOURCC('R', 'P', 'l', 't'): memcpy(out_bytes, "j414s\0\0\0", 8); break;
    case SHENG_SMC_FOURCC('O', 'S', 'K', '0'): memcpy(out_bytes, "ourhardworkbythesewordsguardedpl", 32); break;
    case SHENG_SMC_FOURCC('O', 'S', 'K', '1'): memcpy(out_bytes, "easedontsteal(c)AppleComputerInc", 32); break;
    case SHENG_SMC_FOURCC('F', 'N', 'u', 'm'): out_bytes[0] = 0u; break; // fanless tablet chassis
    case SHENG_SMC_FOURCC('m', 'T', 'P', 'L'): out_bytes[0] = s->thermal_level; break;
    case SHENG_SMC_FOURCC('A', 'C', '-', 'N'): out_bytes[0] = s->external_connected; break;
    case SHENG_SMC_FOURCC('B', 'A', 'T', 'P'): out_bytes[0] = s->battery_present; break;

    case SHENG_SMC_FOURCC('T', 'C', '0', 'P'):
    case SHENG_SMC_FOURCC('T', 'C', '0', 'D'):
    case SHENG_SMC_FOURCC('T', 'p', '0', '9'): sheng_smc_put_flt_le(out_bytes, s->cpu_max_c); break;
    case SHENG_SMC_FOURCC('T', 'C', '0', 'E'):
    case SHENG_SMC_FOURCC('T', 'p', '0', '1'): sheng_smc_put_flt_le(out_bytes, s->cpu_e_c); break;
    case SHENG_SMC_FOURCC('T', 'C', '0', 'F'):
    case SHENG_SMC_FOURCC('T', 'p', '0', '5'): sheng_smc_put_flt_le(out_bytes, s->cpu_p_c); break;
    case SHENG_SMC_FOURCC('T', 'C', '0', 'G'):
    case SHENG_SMC_FOURCC('T', 'p', '0', 'D'): sheng_smc_put_flt_le(out_bytes, s->cpu_x3_c); break;
    case SHENG_SMC_FOURCC('T', 'G', '0', 'P'):
    case SHENG_SMC_FOURCC('T', 'G', '0', 'D'): sheng_smc_put_flt_le(out_bytes, s->gpu_c); break;
    case SHENG_SMC_FOURCC('T', 's', '0', 'P'): sheng_smc_put_flt_le(out_bytes, s->soc_c); break;
    case SHENG_SMC_FOURCC('T', 'W', '0', 'P'): sheng_smc_put_flt_le(out_bytes, s->wifi_c); break;
    case SHENG_SMC_FOURCC('T', 'B', '0', 'T'):
    case SHENG_SMC_FOURCC('T', 'B', '1', 'T'): sheng_smc_put_flt_le(out_bytes, s->batt_c); break;
    case SHENG_SMC_FOURCC('T', 'N', '0', 'P'): sheng_smc_put_flt_le(out_bytes, s->npu_c); break;

    case SHENG_SMC_FOURCC('P', 'S', 'T', 'R'): sheng_smc_put_flt_le(out_bytes, s->sys_w); break;
    case SHENG_SMC_FOURCC('P', 'C', 'P', 'C'): sheng_smc_put_flt_le(out_bytes, s->cpu_w); break;
    case SHENG_SMC_FOURCC('P', 'G', '0', 'R'): sheng_smc_put_flt_le(out_bytes, s->gpu_w); break;
    case SHENG_SMC_FOURCC('P', 'D', 'T', 'R'): sheng_smc_put_flt_le(out_bytes, s->batt_w); break;
    case SHENG_SMC_FOURCC('V', 'D', '0', 'R'):
    case SHENG_SMC_FOURCC('V', 'B', '0', 'R'): sheng_smc_put_flt_le(out_bytes, s->batt_v); break;
    case SHENG_SMC_FOURCC('I', 'D', '0', 'R'):
    case SHENG_SMC_FOURCC('I', 'B', '0', 'R'): sheng_smc_put_flt_le(out_bytes, s->batt_a); break;
    default: break;
    }
}

// Handle selector 2 (kSMCHandleYPCEvent) for any buffer >= 80 bytes (80 legacy or 168 Apple Silicon).
static inline void sheng_smc_handle_call(
    const struct ShengSmcParam *in,
    struct ShengSmcParam *out,
    const struct ShengSmcSnapshot *snap,
    uint32_t *reads,
    uint32_t *infos,
    uint32_t *indices,
    uint32_t *writes,
    uint32_t *misses,
    uint32_t *last_miss)
{
    memset(out, 0, sizeof(*out));
    out->key = in->key;

    switch (in->data8) {
    case SHENG_SMC_CMD_GET_KEY_INFO: {
        if (infos) (*infos)++;
        const struct ShengSmcKeyDef *def = sheng_smc_find_key(in->key);
        if (!def) {
            out->result = SHENG_SMC_RESULT_KEY_NOT_FOUND;
            if (misses) (*misses)++;
            if (last_miss) *last_miss = in->key;
            return;
        }
        out->dataSize = def->size;
        out->dataType = def->type;
        out->dataAttributes = SHENG_SMC_ATTR_READABLE;
        out->result = SHENG_SMC_RESULT_OK;
        return;
    }
    case SHENG_SMC_CMD_READ_KEY: {
        if (reads) (*reads)++;
        const struct ShengSmcKeyDef *def = sheng_smc_find_key(in->key);
        if (!def) {
            out->result = SHENG_SMC_RESULT_KEY_NOT_FOUND;
            if (misses) (*misses)++;
            if (last_miss) *last_miss = in->key;
            return;
        }
        uint8_t val[32];
        sheng_smc_fill_value(def->key, snap, val);
        out->dataSize = def->size;
        out->dataType = def->type;
        out->dataAttributes = SHENG_SMC_ATTR_READABLE;
        memcpy(out->bytes, val, def->size);
        out->result = SHENG_SMC_RESULT_OK;
        return;
    }
    case SHENG_SMC_CMD_GET_KEY_IDX: {
        if (indices) (*indices)++;
        if (in->data32 >= SHENG_SMC_KEY_COUNT) {
            out->result = SHENG_SMC_RESULT_BAD_INDEX;
            return;
        }
        out->key = kShengSmcKeys[in->data32].key;
        out->dataSize = kShengSmcKeys[in->data32].size;
        out->dataType = kShengSmcKeys[in->data32].type;
        out->dataAttributes = SHENG_SMC_ATTR_READABLE;
        out->result = SHENG_SMC_RESULT_OK;
        return;
    }
    case SHENG_SMC_CMD_WRITE_KEY: {
        if (writes) (*writes)++;
        out->result = SHENG_SMC_RESULT_OK; // safe no-op for powerd/thermalmonitord writes
        return;
    }
    default:
        out->result = SHENG_SMC_RESULT_OK;
        return;
    }
}

#ifdef __cplusplus
}
#endif

#endif // SHENG_SMC_KEYS_H
