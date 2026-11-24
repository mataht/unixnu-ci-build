#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "../WifiHotspotIe.h"

int main(void)
{
    uint32_t ch = 0, fl = 0;
    wifi_freq_to_channel(2472, &ch, &fl);
    assert(ch == 13 && fl == 0xau);
    wifi_freq_to_channel(5745, &ch, &fl);
    assert(ch == 149 && fl == 0x12u);
    wifi_freq_to_channel(5975, &ch, &fl);
    assert(ch == 5 && fl == 0x2002u);

    /* Transitional WPA2-PSK (00-0f-ac:2) + WPA3-SAE (00-0f-ac:8) with MFPC|MFPR (0xc0) + RSNXE (tag 244) */
    const uint8_t trans_ie[] = {
        0, 5, 'P', 'h', 'o', 'n', 'e',
        48, 24,
            1, 0,
            0x00, 0x0f, 0xac, 4,
            1, 0, 0x00, 0x0f, 0xac, 4,
            2, 0, 0x00, 0x0f, 0xac, 2, 0x00, 0x0f, 0xac, 8,
            0xc0, 0x00,
        244, 1, 0x20
    };
    uint8_t out[128] = {0};
    uint16_t out_len = wifi_sanitize_hotspot_ies(trans_ie, sizeof(trans_ie), out, sizeof(out));
    assert(out_len == 29);
    assert(out[7] == 48 && out[8] == 20);
    assert(out[21] == 1 && out[22] == 0);
    assert(out[26] == 2);
    assert((out[27] & 0xc0) == 0x80);

    /* ANDROID_METERED / force_hotspot_badge appends 10-byte 00:17:f2:06 Apple Personal Hotspot IE */
    uint16_t hot_len = wifi_sanitize_hotspot_ies_ex(trans_ie, sizeof(trans_ie), out, sizeof(out), true);
    assert(hot_len == 39);
    assert(out[29] == 0xdd && out[30] == 0x08 && out[31] == 0x00 && out[32] == 0x17 && out[33] == 0xf2 && out[34] == 0x06);

    /* Android OUI 00:1a:11 also auto-synthesizes 00:17:f2:06 without duplicating if already present */
    const uint8_t android_ie[] = { 0, 3, 'M', 'i', '1', 0xdd, 0x04, 0x00, 0x1a, 0x11, 0x01 };
    uint16_t and_len = wifi_sanitize_hotspot_ies_ex(android_ie, sizeof(android_ie), out, sizeof(out), false);
    assert(and_len == sizeof(android_ie) + 10);
    assert(out[11] == 0xdd && out[13] == 0x00 && out[14] == 0x17 && out[15] == 0xf2 && out[16] == 0x06);
    uint8_t out2[128] = {0};
    uint16_t idemp_len = wifi_sanitize_hotspot_ies_ex(out, and_len, out2, sizeof(out2), true);
    assert(idemp_len == and_len);

    printf("wifi_hotspot_ie_host: ok
");
    return 0;
}
