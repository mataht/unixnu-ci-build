#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../ShengSmcKeys.h"

int main(void)
{
    assert(sizeof(struct ShengSmcParam) == 168);

    struct ShengSmcSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.cpu_max_c = 65.5f;
    snap.cpu_e_c = 61.0f;
    snap.cpu_p_c = 65.5f;
    snap.cpu_x3_c = 64.25f;
    snap.gpu_c = 58.75f;
    snap.soc_c = 57.5f;
    snap.wifi_c = 55.0f;
    snap.npu_c = 56.0f;
    snap.batt_c = 35.0f;
    snap.thermal_level = 1;
    snap.external_connected = 1;
    snap.battery_present = 1;

    const uint32_t mhz[3] = {1056, 1478, 1324};
    const uint32_t busy[3] = {25, 30, 10};
    sheng_smc_estimate_power(mhz, busy, 1, 4, true, 4315, -840, true, &snap);
    assert(snap.sys_w > 5.0f && snap.sys_w < 6.5f);
    assert(snap.cpu_w > 0.3f && snap.cpu_w < 3.0f);
    assert(snap.gpu_w > 0.15f && snap.gpu_w < 1.0f);

    struct ShengSmcParam in, out;
    uint32_t reads = 0, infos = 0, idxs = 0, writes = 0, misses = 0, last_miss = 0;

    // 1. #KEY big-endian u32
    memset(&in, 0, sizeof(in));
    in.key = SHENG_SMC_FOURCC('#', 'K', 'E', 'Y');
    in.data8 = SHENG_SMC_CMD_READ_KEY;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    assert(out.result == SHENG_SMC_RESULT_OK && out.dataSize == 4 && out.dataType == SHENG_SMC_TYPE_UI32);
    uint32_t count_be = ((uint32_t)out.bytes[0] << 24) | ((uint32_t)out.bytes[1] << 16) |
                        ((uint32_t)out.bytes[2] << 8) | (uint32_t)out.bytes[3];
    assert(count_be == SHENG_SMC_KEY_COUNT);

    // 2. TC0P float little-endian (matches libSMC.dylib SMCParseBytesForNumeric)
    memset(&in, 0, sizeof(in));
    in.key = SHENG_SMC_FOURCC('T', 'C', '0', 'P');
    in.data8 = SHENG_SMC_CMD_GET_KEY_INFO;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    assert(out.result == SHENG_SMC_RESULT_OK && out.dataSize == 4 && out.dataType == SHENG_SMC_TYPE_FLT);

    in.data8 = SHENG_SMC_CMD_READ_KEY;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    float tc0p = 0.0f;
    memcpy(&tc0p, out.bytes, sizeof(float));
    assert(fabsf(tc0p - 65.5f) < 0.001f);

    // 3. Key index enumeration & 0xb8 range terminator
    memset(&in, 0, sizeof(in));
    in.data8 = SHENG_SMC_CMD_GET_KEY_IDX;
    in.data32 = 0;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    assert(out.result == SHENG_SMC_RESULT_OK && out.key == SHENG_SMC_FOURCC('#', 'K', 'E', 'Y'));

    in.data32 = SHENG_SMC_KEY_COUNT;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    assert(out.result == SHENG_SMC_RESULT_BAD_INDEX);

    // 4. Unknown key -> 0x84, write key -> 0x00 no-op
    memset(&in, 0, sizeof(in));
    in.key = SHENG_SMC_FOURCC('Z', 'Z', 'Z', 'Z');
    in.data8 = SHENG_SMC_CMD_READ_KEY;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    assert(out.result == SHENG_SMC_RESULT_KEY_NOT_FOUND && misses == 1 && last_miss == in.key);

    in.data8 = SHENG_SMC_CMD_WRITE_KEY;
    sheng_smc_handle_call(&in, &out, &snap, &reads, &infos, &idxs, &writes, &misses, &last_miss);
    assert(out.result == SHENG_SMC_RESULT_OK && writes == 1);

    printf("smc_keys_host: all %u keys and wire checks passed\n", SHENG_SMC_KEY_COUNT);
    return 0;
}
