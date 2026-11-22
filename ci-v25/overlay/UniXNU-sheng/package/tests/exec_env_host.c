#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static bool match_exec_env(const char *exec_env, const char *path)
{
    const char *colon = strchr(exec_env, ':');
    if (!colon) return false;
    const size_t path_len = strlen(path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const size_t base_len = strlen(base);
    for (const char *tok = exec_env; tok < colon;) {
        const char *bar = tok;
        while (bar < colon && *bar != '|') bar++;
        const size_t tlen = (size_t)(bar - tok);
        if (tlen == 6 && strncmp(tok, "@appex", 6) == 0) {
            if (strstr(path, ".appex/Contents/MacOS/") != NULL ||
                strncmp(path, "/System/Library/ExtensionKit/Extensions/", 40) == 0 ||
                strncmp(path, "/System/Cryptexes/OS/System/Library/ExtensionKit/Extensions/", 60) == 0) {
                (void)path_len;
                return true;
            }
        } else if (tlen > 0 && base_len == tlen) {
            bool eq = true;
            for (size_t ci = 0; ci < tlen; ++ci) {
                if (tok[ci] == '_' ? (base[ci] != '_' && base[ci] != ' ') : (base[ci] != tok[ci])) {
                    eq = false;
                    break;
                }
            }
            if (eq) return true;
        }
        tok = bar + (bar < colon && *bar == '|' ? 1 : 0);
    }
    return false;
}

int main(void)
{
    const char *spec = "iconservicesagent|iconservicesd|System_Settings|Finder|Dock|ControlCenter|SystemUIServer|loginwindow|@appex:FORCE_SOFTWARE_RENDERER=1";
    assert(strchr(spec, ' ') == NULL); /* boot-arg token MUST NOT contain any space */
    assert(match_exec_env(spec, "/System/Library/CoreServices/iconservicesagent"));
    assert(match_exec_env(spec, "/System/Applications/System Settings.app/Contents/MacOS/System Settings"));
    assert(match_exec_env(spec, "/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow"));
    assert(match_exec_env(spec, "/System/Library/ExtensionKit/Extensions/Appearance.appex/Contents/MacOS/Appearance"));
    assert(match_exec_env(spec, "/System/Applications/System Settings.app/Contents/PlugIns/Sidebar.appex/Contents/MacOS/Sidebar"));
    assert(!match_exec_env(spec, "/System/Cryptexes/App/System/Applications/Safari.app/Contents/MacOS/Safari"));
    assert(!match_exec_env(spec, "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer"));
    printf("exec_env_host: ok
");
    return 0;
}
