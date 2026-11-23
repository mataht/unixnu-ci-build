// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// The partition that holds the root image, the persistent Data store and the diagnostic log.
//
// sheng CN 16 GB / 1 TB with Android resident (local patch series hw-build/upstream/patches-sheng-cn): GPT entry 32
// "macossystem" on LUN 0, never Android's "userdata". The driver takes a partition only when its name and its whole LBA range
// equal these values, so a GPT other than the one this build was made for (for example the 64 GiB macossystem of the macOS 26.5
// period, LBA 45054208..61831423) is refused before any manifest is read or any write window is opened.
// A test may define the three values itself before it includes RootLayout.h (SHENG_UFS_ROOT_TEST).
#ifndef SHENG_UFS_ROOT_PARTITION
#define SHENG_UFS_ROOT_PARTITION "macossystem"
#endif
#ifndef SHENG_UFS_ROOT_FIRST_LBA
#define SHENG_UFS_ROOT_FIRST_LBA 37714176ULL     // 4096-byte LBAs on LUN 0
#endif
#ifndef SHENG_UFS_ROOT_LAST_LBA
#define SHENG_UFS_ROOT_LAST_LBA 257745663ULL     // inclusive; 220031488 blocks, 839.35 GiB
#endif

static constexpr bool rootPartitionNameIs (const char *name, const char *other)
{
  for (unsigned i = 0;; ++i) {
    if (name[i] != other[i]) return false;
    if (!name[i]) return true;
  }
}

// Android's data partition is never a root partition on this board, whatever a build sets.
static_assert (!rootPartitionNameIs (SHENG_UFS_ROOT_PARTITION, "userdata"), "the root partition must not be Android's userdata");
static_assert (sizeof (SHENG_UFS_ROOT_PARTITION) > 1 && sizeof (SHENG_UFS_ROOT_PARTITION) <= 36, "a GPT name has 1..35 characters");
#ifndef SHENG_UFS_ROOT_TEST
static_assert (SHENG_UFS_ROOT_FIRST_LBA <= SHENG_UFS_ROOT_LAST_LBA && SHENG_UFS_ROOT_LAST_LBA <= 0xffffffffULL,
               "the root partition must lie inside READ(10)/WRITE(10) range");
#endif
