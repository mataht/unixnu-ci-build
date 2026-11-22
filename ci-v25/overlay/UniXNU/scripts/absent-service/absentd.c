// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stand-in for board-incompatible daemons (native_deny_exec). Non-launchd CLI invocations (e.g. early-boot
// pid-5 `spindump -shutdownstall 2 -timelimit 5` or child `seputil` helper without XPC_SERVICE_NAME) exit 0
// immediately; launchd jobs (XPC_SERVICE_NAME set) check in on all MachServices declared either in standalone
// LaunchDaemons/LaunchAgents plists OR in macOS 27's compiled `/System/Library/xpc/launchd.plist` (such as
// `com.apple.lskdd` and `com.apple.fairplayd`) and stay alive in dispatch_main().
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <servers/bootstrap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

static bool is_raw_mig_service(const char *name)
{
    if (!name) return false;
    return !strcmp(name, "com.apple.libquitd") ||
           !strcmp(name, "com.apple.spinreporterd");
}

static void reply_or_destroy_mig(mach_msg_header_t *hdr)
{
    const mach_msg_bits_t remote_type = MACH_MSGH_BITS_REMOTE(hdr->msgh_bits);
    if (remote_type == MACH_MSG_TYPE_MOVE_SEND_ONCE && MACH_PORT_VALID(hdr->msgh_remote_port)) {
        const mach_port_t reply_port = hdr->msgh_remote_port;
        const mach_msg_id_t req_id = hdr->msgh_id;
        hdr->msgh_remote_port = MACH_PORT_NULL;
        hdr->msgh_bits &= ~MACH_MSGH_BITS_REMOTE_MASK;
        mach_msg_destroy(hdr);

        mig_reply_error_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        reply.Head.msgh_size = (mach_msg_size_t)sizeof(reply);
        reply.Head.msgh_remote_port = reply_port;
        reply.Head.msgh_local_port = MACH_PORT_NULL;
        reply.Head.msgh_id = req_id + 100;
        reply.NDR = NDR_record;
        reply.RetCode = MIG_BAD_ID;
        kern_return_t skr = mach_msg(&reply.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                                     reply.Head.msgh_size, 0, MACH_PORT_NULL, 10, MACH_PORT_NULL);
        if (skr != MACH_MSG_SUCCESS && skr != MACH_SEND_INVALID_DEST) {
            mach_msg_destroy(&reply.Head);
        }
        return;
    }
    mach_msg_destroy(hdr);
}

static void drain_mig_port(mach_port_t port)
{
    for (;;) {
        union {
            mach_msg_header_t hdr;
            uint8_t buf[4096];
        } msg;
        memset(&msg, 0, sizeof(msg.hdr));
        kern_return_t kr = mach_msg(&msg.hdr,
                                    MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_LARGE,
                                    0, (mach_msg_size_t)sizeof(msg), port, 0, MACH_PORT_NULL);
        if (kr == MACH_RCV_TOO_LARGE) {
            size_t big_sz = (size_t)msg.hdr.msgh_size + 1024u;
            mach_msg_header_t *big = (mach_msg_header_t *)calloc(1, big_sz);
            if (!big) break;
            if (mach_msg(big, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                         0, (mach_msg_size_t)big_sz, port, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS) {
                reply_or_destroy_mig(big);
            }
            free(big);
            continue;
        }
        if (kr != MACH_MSG_SUCCESS) break;
        reply_or_destroy_mig(&msg.hdr);
    }
}

static void refuse_mig(const char *name)
{
    mach_port_t port = MACH_PORT_NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    kern_return_t kr = bootstrap_check_in(bootstrap_port, name, &port);
#pragma clang diagnostic pop
    if (kr != KERN_SUCCESS || !MACH_PORT_VALID(port)) return;
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, port, 0, dispatch_get_main_queue());
    if (!src) return;
    dispatch_source_set_event_handler(src, ^{
        drain_mig_port(port);
    });
    dispatch_resume(src);
}

static void refuse(const char *name)
{
    if (!name || !*name) return;
    if (is_raw_mig_service(name)) {
        refuse_mig(name);
        return;
    }
    xpc_connection_t listener = xpc_connection_create_mach_service(name, dispatch_get_main_queue(), XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) return;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_set_event_handler((xpc_connection_t)peer, ^(xpc_object_t event) { (void)event; });
        xpc_connection_activate((xpc_connection_t)peer);
        xpc_connection_cancel((xpc_connection_t)peer);
    });
    xpc_connection_activate(listener);
}

struct RefuseCtx {
    size_t count;
};

static void refuse_each(const void *key, const void *value, void *context)
{
    (void)value;
    struct RefuseCtx *ctx = (struct RefuseCtx *)context;
    char name[256];
    if (CFGetTypeID(key) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)key, name, sizeof name, kCFStringEncodingUTF8)) {
        refuse(name);
        if (ctx) ++ctx->count;
    }
}

static CFPropertyListRef read_plist_file(const char *path)
{
    CFPropertyListRef plist = NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CFReadStreamRef stream = url ? CFReadStreamCreateWithFile(NULL, url) : NULL;
    if (stream && CFReadStreamOpen(stream)) {
        plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
        CFReadStreamClose(stream);
    }
    if (stream) CFRelease(stream);
    if (url) CFRelease(url);
    return plist;
}

static size_t apply_job_dict(CFDictionaryRef job_dict)
{
    struct RefuseCtx ctx = {0};
    if (!job_dict || CFGetTypeID(job_dict) != CFDictionaryGetTypeID()) return 0;
    CFTypeRef services = CFDictionaryGetValue(job_dict, CFSTR("MachServices"));
    if (services && CFGetTypeID(services) == CFDictionaryGetTypeID()) {
        CFDictionaryApplyFunction((CFDictionaryRef)services, refuse_each, &ctx);
    }
    return ctx.count;
}

static size_t lookup_compiled_launchd_plist(const char *label)
{
    static const char * const kCompiledPlists[] = {
        "/System/Library/xpc/launchd.plist",
        "/System/Cryptexes/OS/System/Library/xpc/launchd.plist",
    };
    static const CFStringRef kSections[] = {
        CFSTR("LaunchDaemons"),
        CFSTR("LaunchAgents"),
    };
    CFStringRef cf_label = CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8);
    if (!cf_label) return 0;
    size_t total = 0;
    for (size_t i = 0; i < sizeof(kCompiledPlists) / sizeof(kCompiledPlists[0]) && total == 0; ++i) {
        CFPropertyListRef root = read_plist_file(kCompiledPlists[i]);
        if (!root) continue;
        if (CFGetTypeID(root) == CFDictionaryGetTypeID()) {
            for (size_t s = 0; s < sizeof(kSections) / sizeof(kSections[0]); ++s) {
                CFTypeRef sec = CFDictionaryGetValue((CFDictionaryRef)root, kSections[s]);
                if (sec && CFGetTypeID(sec) == CFDictionaryGetTypeID()) {
                    CFTypeRef entry = CFDictionaryGetValue((CFDictionaryRef)sec, cf_label);
                    if (entry && CFGetTypeID(entry) == CFDictionaryGetTypeID()) {
                        total += apply_job_dict((CFDictionaryRef)entry);
                    }
                }
            }
        }
        CFRelease(root);
    }
    CFRelease(cf_label);
    return total;
}

static void apply_builtin_fallbacks(const char *label)
{
    if (!strcmp(label, "com.apple.lskdd")) {
        refuse("com.apple.lskdd");
    } else if (!strcmp(label, "com.apple.fairplayd") ||
               !strcmp(label, "com.apple.fairplayd.Galactic")) {
        refuse("com.apple.fairplayd");
        refuse("com.apple.fairplayd.xpc");
        refuse("com.apple.fpsd");
        refuse("com.apple.arcade.fpsd");
    } else if (!strcmp(label, "com.apple.spindump")) {
        refuse("com.apple.spindump");
        refuse("com.apple.libquitd");
        refuse("com.apple.spinreporterd");
    }
}

int main(int argc, char **argv)
{
    const char *arg0 = (argc > 0 && argv && argv[0]) ? argv[0] : NULL;
    const char *arg0_base = arg0 ? strrchr(arg0, '/') : NULL;
    arg0_base = arg0_base ? arg0_base + 1 : arg0;

    // Check XPC_SERVICE_NAME first so launchd-managed jobs never exit 0 into a 10s ThrottleInterval loop.
    const char *label = getenv("XPC_SERVICE_NAME");
    if (!label || !*label || (label[0] == '0' && !label[1])) {
        return 0;
    }
    // Non-launchd CLI flags such as `spindump -shutdownstall 2 -timelimit 5` or child `seputil` invocations exit 0.
    if (getppid() != 1) {
        if (arg0_base && !strcmp(arg0_base, "seputil")) return 0;
        for (int i = 1; i < argc; ++i) {
            if (argv[i] && !strcmp(argv[i], "-shutdownstall")) return 0;
        }
    } else if (arg0_base && !strcmp(arg0_base, "seputil") && strcmp(label, "com.apple.prngseedd") != 0) {
        return 0;
    }

    static const char * const kJobDirs[] = {
        "/System/Library/LaunchDaemons",
        "/System/Library/LaunchAgents",
        "/System/Cryptexes/OS/System/Library/LaunchDaemons",
        "/System/Cryptexes/OS/System/Library/LaunchAgents",
    };
    size_t claimed = 0;
    CFPropertyListRef job = NULL;
    for (size_t i = 0; i < sizeof(kJobDirs) / sizeof(kJobDirs[0]) && !job; ++i) {
        char path[512];
        if (snprintf(path, sizeof path, "%s/%s.plist", kJobDirs[i], label) >= (int)sizeof path) continue;
        job = read_plist_file(path);
    }
    if (job) {
        if (CFGetTypeID(job) == CFDictionaryGetTypeID()) {
            claimed += apply_job_dict((CFDictionaryRef)job);
        }
        CFRelease(job);
    }
    if (claimed == 0) {
        claimed += lookup_compiled_launchd_plist(label);
    }
    if (claimed == 0) {
        apply_builtin_fallbacks(label);
    }
    dispatch_main();
}
