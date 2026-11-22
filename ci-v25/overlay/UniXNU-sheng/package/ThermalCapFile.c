// SPDX-License-Identifier: GPL-3.0-or-later
// The thermal cap log file, /private/var/log/sheng-thermal.log on the Data volume, through the VFS (BSD flags, as AdspFirmware.c).
// The file is opened for each write and closed again: an open vnode would keep the Data volume busy at shutdown. Before the Data volume
// is mounted the open fails and the caller keeps its lines. At 1 MiB the file becomes sheng-thermal.log.1: the kernel has no rename by path
// for a driver, so it is copied, then cut.
#include <stdint.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/errno.h>
#include <IOKit/IOLib.h>

int sheng_thermal_cap_file_append(const char *data, unsigned bytes);

static const char kLogPath[] = "/private/var/log/sheng-thermal.log";
static const char kOldLogPath[] = "/private/var/log/sheng-thermal.log.1";
enum {kRotateBytes = 1 << 20, kCopyBytes = 16384};

static int write_all(vnode_t vp, char *data, int bytes, off_t offset, int flags, vfs_context_t context)
{
    int resid = 0;
    const int error = vn_rdwr(UIO_WRITE, vp, data, bytes, offset, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT | flags, vfs_context_ucred(context),
                              &resid, vfs_context_proc(context));
    return !error && resid ? EIO : error;
}

static void rotate(vfs_context_t context)
{
    char *buffer = (char *)IOMallocData(kCopyBytes);
    vnode_t from = NULLVP, to = NULLVP;
    int error = buffer ? vnode_open(kLogPath, FREAD | O_NOFOLLOW, 0, 0, &from, context) : ENOMEM;
    if (!error) {
        error = vnode_open(kOldLogPath, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW, 0644, 0, &to, context);
        if (!error) {
            off_t at = 0;
            for (;;) {
                int resid = 0;
                error = vn_rdwr(UIO_READ, from, buffer, kCopyBytes, at, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(context), &resid,
                                vfs_context_proc(context));
                const int got = kCopyBytes - resid;
                if (error || got <= 0) break;
                error = write_all(to, buffer, got, at, 0, context);
                if (error) break;
                at += got;
            }
            if (!error) error = VNOP_FSYNC(to, MNT_WAIT, context);
            vnode_close(to, FWRITE, context);
        }
        vnode_close(from, FREAD, context);
    }
    if (buffer) IOFreeData(buffer, kCopyBytes);
    if (error) IOLog("ThermalCap: log rotation copy failed: %d\n", error);
    // Cut the log either way, so that it stays bounded.
    vnode_t cut = NULLVP;
    if (vnode_open(kLogPath, O_TRUNC | FWRITE | O_NOFOLLOW, 0, 0, &cut, context) == 0) vnode_close(cut, FWRITE, context);
}

// Appends bytes to the log and syncs it. Returns 0 when everything is on the disk, otherwise the error; nothing is kept here.
int sheng_thermal_cap_file_append(const char *data, unsigned bytes)
{
    if (!bytes) return 0;
    vfs_context_t context = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = vnode_open(kLogPath, O_CREAT | O_APPEND | FWRITE | O_NOFOLLOW, 0644, 0, &vp, context);
    if (error) return error;
    struct vnode_attr attributes;
    VATTR_INIT(&attributes);
    VATTR_WANTED(&attributes, va_data_size);
    const int rotateAfter = vnode_getattr(vp, &attributes, context) == 0 && (uint64_t)attributes.va_data_size >= (uint64_t)kRotateBytes;
    error = write_all(vp, (char *)(uintptr_t)data, (int)bytes, 0, IO_APPEND, context);
    if (!error) error = VNOP_FSYNC(vp, MNT_WAIT, context);
    const int closed = vnode_close(vp, FWRITE, context);
    if (!error) error = closed;
    if (error) return error;
    if (rotateAfter) rotate(context);
    return 0;
}
