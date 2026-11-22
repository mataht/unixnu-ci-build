#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XNU_TARGET_OS_OSX 1
static char *test_strnstr(const char *s, const char *find, size_t slen)
{
    size_t flen = strlen(find);
    if (!flen) return (char *)s;
    for (size_t i = 0; i + flen <= slen && s[i]; ++i) {
        if (memcmp(s + i, find, flen) == 0) return (char *)(s + i);
    }
    return NULL;
}
#define strnstr test_strnstr

static bool
appsync_user_code_path(const char *path)
{
    if (path == NULL || path[0] != '/') return false;
    const size_t full_len = strlen(path);
    if (strnstr(path, "/../", full_len) || strnstr(path, "/./", full_len) ||
        (full_len >= 3 && memcmp(path + full_len - 3, "/..", 3) == 0) ||
        (full_len >= 2 && memcmp(path + full_len - 2, "/.", 2) == 0)) {
        return false;
    }
    const char *p = path;
    if (strncmp(p, "/System/Volumes/Data/", 21) == 0) p += 20;
    if (strncmp(p, "/System/", 8) == 0 ||
        strncmp(p, "/private/var/ngpu/", 18) == 0 ||
        strncmp(p, "/var/ngpu/", 10) == 0 ||
        strncmp(p, "/private/var/db/", 16) == 0 ||
        strncmp(p, "/var/db/", 8) == 0 ||
        strncmp(p, "/Library/GPUBundles/", 20) == 0 ||
        strncmp(p, "/Applications/Shader Cache.app/", 31) == 0) {
        return false;
    }
    if (strncmp(p, "/Applications/", 14) == 0 ||
        strncmp(p, "/Users/", 7) == 0 ||
        strncmp(p, "/opt/", 5) == 0 ||
        strncmp(p, "/usr/local/", 11) == 0 ||
        strncmp(p, "/Library/Application Support/", 29) == 0 ||
        strncmp(p, "/Library/PrivilegedHelperTools/", 31) == 0 ||
        strncmp(p, "/Library/Caches/", 16) == 0 ||
        strncmp(p, "/Library/Frameworks/", 20) == 0 ||
        strncmp(p, "/private/tmp/", 13) == 0 ||
        strncmp(p, "/tmp/", 5) == 0 ||
        strncmp(p, "/private/var/folders/", 21) == 0 ||
        strncmp(p, "/var/folders/", 13) == 0 ||
        strncmp(p, "/private/var/tmp/", 17) == 0 ||
        strncmp(p, "/var/tmp/", 9) == 0 ||
        strncmp(p, "/Volumes/", 9) == 0) {
        return true;
    }
    return false;
}

int main(void)
{
    assert(appsync_user_code_path("/Applications/ChatGPT.app/Contents/MacOS/ChatGPT"));
    assert(appsync_user_code_path("/System/Volumes/Data/Applications/ExampleIDE.app/Contents/MacOS/ExampleIDE"));
    assert(appsync_user_code_path("/Users/sheng/.ExampleIDE/bin/rg"));
    assert(appsync_user_code_path("/opt/homebrew/bin/git"));
    assert(appsync_user_code_path("/Volumes/ChatGPT/ChatGPT.app/Contents/MacOS/ChatGPT"));
    assert(appsync_user_code_path("/private/var/folders/zz/abc/T/ShipIt/Update.app/Contents/MacOS/Update"));

    assert(!appsync_user_code_path("/System/Cryptexes/App/System/Applications/Safari.app/Contents/MacOS/Safari"));
    assert(!appsync_user_code_path("/System/Applications/System Settings.app/Contents/MacOS/System Settings"));
    assert(!appsync_user_code_path("/Applications/Shader Cache.app/Contents/MacOS/ShaderCache"));
    assert(!appsync_user_code_path("/System/Volumes/Data/Applications/Shader Cache.app/Contents/MacOS/ShaderCache"));
    assert(!appsync_user_code_path("/private/var/ngpu/bin/unixnu-absentd"));
    assert(!appsync_user_code_path("/Library/GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal"));
    assert(!appsync_user_code_path("/System/Volumes/Preboot/foo"));
    assert(!appsync_user_code_path("/Applications/../System/Library/foo"));

    printf("appsync_path_host: ok
");
    return 0;
}
