// SPDX-License-Identifier: MIT
// Explicit temporary bring-up policy for an unavailable watchdog backend.
// Runs after writable scratch Data mounts, before the unchanged launchd exec.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
 "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[]="<key>com.apple.watchdogd</key><true/>";
// Bring-up isolation: hardware stacks show identityservicesd, contextstored and locationd in a
// cycle of synchronous XPC waits. A disabled job makes its Mach service lookups fail instead of hang.
// disable-jobs=<label>[,<label>...] (restricted label characters, bounded length).
static const char policy_tail[]="</dict></plist>\n";
// lite=1: jobs that only reach for the network, iCloud or the photo library and have nothing to do on this tablet (no radios, no accounts),
// all of which start at the first boot of a fresh Data and compete with SpringBoard for the CPU and for memory. The first three are the
// cycle of synchronous XPC waits the isolation work found. Labels are the ones in the target's own LaunchDaemons.
static const char lite_jobs[]=
 "com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,"
 "com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.mediastream.mstreamd,com.apple.icloud.searchpartyd,"
 "com.apple.findmy.findmybeaconingd,com.apple.familycircled,com.apple.financed,com.apple.softwareupdateservicesd,"
 "com.apple.ndoagent,com.apple.biomesyncd,com.apple.email.maild,com.apple.amsengagementd,com.apple.adid,com.apple.eyereliefd,"
 // Crash or respawn loops seen on hardware (int-176): fairplayd.A2 is KeepAlive and exits at once (22 launches in 6 minutes), passd crashes,
 // obliteration is a data-wipe daemon with nothing to do here. iconservicesagent also dies in CI::GLContext::init, but disabling it leaves
 // the icons missing (seen on hardware), so the usual lite policy keeps it enabled. coreduetd keeps one thread above 50 % of a core for the whole boot
 // (the kernel's "burning CPU" report in i480 to i556), holding a big core and the P domain at its top clock.
 "com.apple.fairplayd.A2,com.apple.passd,com.apple.mobile.obliteration,com.apple.coreduetd,"
 // Advice and statistics services with nothing to advise on here (untagged-3f2bd680: PerfPowerServices alone held 1.8 of 7 CPUs while it
 // wrote its power database, BackgroundShortcutRunner 0.3 to 0.5 all boot, duetexpertd 0.17 in Safari). Temporarily disabled to measure.
 "com.apple.PerfPowerServices,com.apple.PerfPowerServicesExtended,com.apple.perfpowermetricd,com.apple.siriinferenced,"
 "com.apple.siriknowledged,com.apple.siriactionsd,com.apple.duetexpertd,com.apple.modelcatalogd";
// The board's always-on switches (pexpert/gen/bootargs.c reads them after the command line, which wins), so the boot loader's line,
// at most 1000 bytes, carries only what changes between images: the staged system apps' view over /Applications (their icons), the
// data-protection class kept on the unencrypted Data volume (PosterBoard's wallpaper store), and the camera daemon off (no camera:
// it held a GPU client and wrote denied trace files every 5 s).
// The scheduler hygiene checks of a development kernel are off, as a shipping (release) kernel has none. Their default mode (2)
// panics: an interrupts-off window of 786 us in a work queue thread's priority change panicked the board (untagged-203fd45bf).
// Mode 1 (trace) reports only to kdebug and DTrace, which nothing on this board records, and keeps the whole measurement: every outermost
// preemption disable masks interrupts and takes a three-frame backtrace and a timestamp, every matching enable another timestamp
// (osfmk/arm/preemption_disable.c). The interrupt check of xnu-12377 takes a timestamp at every interrupt mask and unmask in any
// mode, as long as its static key is on (osfmk/arm/machine_routines_common.c; xnu-10063 takes it only while a mode is on). So both
// modes are 0 and validation_disables turns the two static keys off (0x10000 preemption, 0x40 interrupts; this takes a kernel whose
// static keys read the built-in arguments, otherwise only the modes apply). Its bit 0x1000 stops the development kernel from
// zero-filling every other page a process releases with madvise (madvise_free_debug_sometimes, osfmk/vm/vm_object.c), which a
// release kernel never does. The checks that find kernel defects (assertions 0x800, mapping list 0x4, process reference tracking
// 0x20000) stay on.
// The command line wins: validation_disables=0 sched_preemption_disable_debug_mode=1 interrupt-masked-debug-mode=1 traces again.
// macOS 27.3 also takes here the switches only its kernel reads, which do not fit the command line any more: userspace reads the
// scaled timebase itself, signatures are authenticated per block at first use, the cover touchpad takes a lone contact, the
// scheduler's two-cluster matrix, the boot payload's memory returned, the thermal probe (sampled every 100 ms) and the level macOS
// reads from it, and the board clock. The Metal plugin reads its own switches from kern.bootargs, so they stay on the command line.
char PE_builtin_boot_args[]="owner-staged-apps=1 owner-dataprotect=1 disable-jobs=com.apple.cameracaptured sched_preemption_disable_debug_mode=0 interrupt-masked-debug-mode=0 validation_disables=0x11040"
#if XNU_TARGET_OS_OSX
    " timebase-user=1 cs-lazy=1 keyboard-lone=1 armcpu-edge=1 boot-payload-release=1 thermal=1 thermal-ms=100 thermal-level=1 thermal-cap=3 log-power=1 board-clock=7 acm_fastsim=1 gpu-pool=0 clpc=1 smc=1 cdsp=0 log-video=1 log-wifi=1 appsync=1 exec-env=iconservicesagent|iconservicesd|System_Settings|Finder|Dock|ControlCenter|SystemUIServer|loginwindow|Terminal|com.apple.appkit.xpc.openAndSavePanelService|QuickLookUIService|@appex:FORCE_SOFTWARE_RENDERER=1"
#endif
    ;
#if XNU_TARGET_OS_OSX
// macOS 27 LaunchDaemons whose Mach services require Apple SEP / FairPlay / LSKD silicon not present on sheng:
// keeping them enabled leaves their Mach ports published while fairplayd exits (~30 ms) and lskdd crashes (~25 ms,
// 0xff595c00) without draining queued messages, wedging Games.app / TV.app in synchronous XPC (and blocking
// loginwindow AppleEvent quit at Shut Down) while respawning fairplayd/lskdd/seputil every 10 s. Disabling them in
// disabled.plist makes bootstrap lookups fail cleanly with BOOTSTRAP_UNKNOWN_SERVICE instead of hanging.
// keep-sep-jobs=1 on the boot command line re-enables them for isolation comparison.
static const char macos_unavailable_hw_jobs[]=
 "com.apple.fairplayd,com.apple.fairplaydeviceidentityd,com.apple.fpassetmanagerd,"
 "com.apple.lskdd,com.apple.prngseedd,com.apple.adid,com.apple.fairplayd.A2,com.apple.fpsd,"
 "com.apple.biometrickitd";
// System LaunchDaemons and per-UID GUI LaunchAgents that expect Apple ANE / Apple Silicon power telemetry or run
// continuous background ML / media-vision / Spotlight semantic-embedding jobs on CPU fallback (BootLog-08 proved
// 88-100% P-core + 65-100% X3 userland saturation and ~8.95 W board power at idle desktop, vs 57 C / 4.3 W battery
// when userland paused). Interactive desktop features (Wi-Fi, Safari, Finder, Games, TV, Siri assistantd/siriappintentsd,
// chronod widgets, iconservicesagent, and mds/mds_stores filename Spotlight) remain enabled.
// keep-bg-jobs=1 on the boot command line clears both system and per-UID background overrides for A/B comparison.
static const char macos_bg_daemon_jobs[]=
 "com.apple.PerfPowerServices,com.apple.PerfPowerServicesExtended,com.apple.perfpowermetricd,"
 "com.apple.powerdatad,com.apple.coreduetd,com.apple.aned,com.apple.aneuserd,com.apple.modelcatalogd,"
 "com.apple.tailspind,com.apple.ReportMemoryException,com.apple.metadata.mds.spindump,"
 "com.apple.softwareupdated,com.apple.mobileassetd,com.apple.mobile.softwareupdated";
static const char macos_bg_agent_jobs[]=
 "com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.duetexpertd,"
 "com.apple.siriinferenced,com.apple.siriknowledged,com.apple.siriactionsd,"
 "com.apple.ModelCatalogAgent,com.apple.generativeexperiencesd,com.apple.GenerativeFunctions.agentstored,"
 "com.apple.intelligenceplatformd,com.apple.intelligenceflowd,com.apple.intelligencecontextd,com.apple.intelligencetasksd,"
 "com.apple.knowledge-agent,com.apple.knowledgeconstructiond,com.apple.proactived,com.apple.proactiveeventtrackerd,"
 "com.apple.visualintelligenced,com.apple.callintelligenced,com.apple.biomesyncd,"
 "com.apple.spotlightknowledged,com.apple.spotlightknowledged.importer,com.apple.spotlightknowledged.updater,"
 "com.apple.spindump_agent,com.apple.sysdiagnose_agent,com.apple.triald,"
 "com.apple.amsengagementd,com.apple.SoftwareUpdateNotificationManager,com.apple.assetsubscriptiond";
#endif
static char policy[4096],jobs[512];
volatile int sheng_watchdog_policy_status=-1;

// setup=1 or data-reset=2 (the install entry's "show Hello" choice): remove Setup's completion flags once, so this boot shows the Hello page.
// iPadOS writes them again when Setup finishes, so later boots do not show it. Absent files are fine.
static int sheng_boot_setup_reset(proc_t p,uint64_t scratch) {
 static const char *const files[]={
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist"};
 for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i) {
  int32_t result=0;
  int error=copyout(files[i],scratch,strlen(files[i])+1);
  if(error)return error;
  struct unlink_args rm={.path=scratch};
  error=unlink(p,&rm,&result);
  if(error && error!=ENOENT)return error;
 }
 return 0;
}

// One comma-separated label at *c into the policy as a disabled key (deduplicated); advances *c past it.
static int append_job(const char **cursor) {
 const char *c=*cursor,*e=c;while(*e && *e!=',')++e;
 if(e>c) {
  char tag[160];
  if((size_t)(e-c)+12>=sizeof(tag))return EINVAL;
  memcpy(tag,"<key>",5);
  memcpy(tag+5,c,(size_t)(e-c));
  memcpy(tag+5+(size_t)(e-c),"</key>",7);
  if(!strnstr(policy,tag,sizeof(policy))) {
   if(strlcat(policy,tag,sizeof(policy))>=sizeof(policy))return EINVAL;
   if(strlcat(policy,"<true/>",sizeof(policy))>=sizeof(policy))return EINVAL;
  }
 }
 *cursor=*e?e+1:e;return 0;
}

// Write the current NUL-terminated policy[] buffer atomically in-place to path (refusing symlinks, enforcing 0:0 0644).
// Uses scratch+0x000 for path, scratch+0x200..+0x3f0 for chunked data (preserving cache-mount env slots at +0x400..+0x600),
// and scratch+0x800 for fstat64 verification.
static int write_policy_file(proc_t p,uint64_t scratch,const char *path) {
 int error=0,fd=-1;int32_t result=0;
 if((error=copyout(path,scratch,strlen(path)+1)))return error;
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))return error;
 fd=result;
 for(size_t sent=0,total=strlen(policy);sent<total;) {
  size_t piece=total-sent<0x1f0?total-sent:0x1f0;
  if((error=copyout(policy+sent,scratch+0x200,piece)))goto out;
  struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=piece};
  user_ssize_t written=0;
  if((error=write(p,&wr,&written)))goto out;
  if(written!=(user_ssize_t)piece){error=EIO;goto out;}
  sent+=piece;
 }
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto out;
 struct fchmod_args fmode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&fmode,&result)))goto out;
 struct fsync_args sync={.fd=fd};
 if((error=fsync(p,&sync,&result)))goto out;
 struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if((error=sys_fstat64(p,&sa,&result)) || (error=copyin(sa.ub,&st,sizeof(st))))goto out;
 if(st.st_uid!=0 || st.st_gid!=0 || !S_ISREG(st.st_mode) || (st.st_mode&0777)!=0644 || st.st_size!=(off_t)(strlen(policy)))error=EINVAL;
out:
 if(fd>=0){struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 return error;
}

int sheng_boot_watchdog_policy(void *process, uint64_t scratch) {
 proc_t p=process;
 int enabled=0,error=0;int32_t result=0;
 PE_parse_boot_argn("disable-watchdogd",&enabled,sizeof(enabled));
 if(!PE_parse_boot_argn("disable-jobs",jobs,sizeof(jobs)))jobs[0]=0;jobs[sizeof(jobs)-1]=0;
 int lite=0;PE_parse_boot_argn("lite",&lite,sizeof(lite));if(lite!=1 && lite!=2)lite=0;
 if(enabled!=1)enabled=0;
 for(const char *c=jobs;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'||*c==','))return EINVAL;
 int setup=0,reset=0;
 PE_parse_boot_argn("setup",&setup,sizeof(setup));
 PE_parse_boot_argn("data-reset",&reset,sizeof(reset));   // 2 = erase Data and show the Hello page; 1 = erase Data, Setup stays skipped
 if(reset==2)setup=1;
 if(setup==1 && (error=sheng_boot_setup_reset(p,scratch))) {
  sheng_watchdog_policy_status=error;
  printf("sheng setup flags reset failed: %d\n",error);
  return error;
 }
 if(setup==1)printf("sheng setup flags reset: Hello page on this boot\n");
 int keep_sep_jobs=1,keep_bg_jobs=1;
#if XNU_TARGET_OS_OSX
 keep_sep_jobs=0;
 PE_parse_boot_argn("keep-sep-jobs",&keep_sep_jobs,sizeof(keep_sep_jobs));
 if(keep_sep_jobs!=1)keep_sep_jobs=0;
 keep_bg_jobs=0;
 PE_parse_boot_argn("keep-bg-jobs",&keep_bg_jobs,sizeof(keep_bg_jobs));
 if(keep_bg_jobs!=1)keep_bg_jobs=0;
#else
 if(!enabled && !jobs[0] && !lite && keep_sep_jobs && keep_bg_jobs)return 0;
#endif
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 if(enabled)strlcat(policy,policy_watchdogd,sizeof(policy));
#if XNU_TARGET_OS_OSX
 if(!keep_sep_jobs) {
  for(const char *c=macos_unavailable_hw_jobs;*c;) {if((error=append_job(&c)))return error;}
 }
 if(!keep_bg_jobs) {
  for(const char *c=macos_bg_daemon_jobs;*c;) {if((error=append_job(&c)))return error;}
 }
#endif
 if(lite)for(const char *c=lite_jobs;*c;) {if((error=append_job(&c)))return error;}
 if(lite==2) {
  const char *icon_agent="com.apple.iconservices.iconservicesagent";
  if((error=append_job(&icon_agent)))return error;
 }
 for(const char *c=jobs;*c;) {if((error=append_job(&c)))return error;}
 if(strlcat(policy,policy_tail,sizeof(policy))>=sizeof(policy))return EINVAL;
 const char *directory="/private/var/db/com.apple.xpc.launchd";
 if((error=copyout(directory,scratch,strlen(directory)+1)))goto done;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);
 if(error && error!=EEXIST)goto done;
 if((error=write_policy_file(p,scratch,"/private/var/db/com.apple.xpc.launchd/disabled.plist")))goto done;
#if XNU_TARGET_OS_OSX
 // Reuse policy[4096] (zero extra __BSS) to write per-UID GUI LaunchAgent overrides (501 primary user, 502 secondary);
 // when keep-bg-jobs=1, write an empty <dict></dict> so persistent Data never retains stale agent disables.
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 if(!keep_bg_jobs) {
  for(const char *c=macos_bg_agent_jobs;*c;) {if((error=append_job(&c)))goto done;}
 }
 if(strlcat(policy,policy_tail,sizeof(policy))>=sizeof(policy)){error=EINVAL;goto done;}
 if((error=write_policy_file(p,scratch,"/private/var/db/com.apple.xpc.launchd/disabled.501.plist")))goto done;
 if((error=write_policy_file(p,scratch,"/private/var/db/com.apple.xpc.launchd/disabled.502.plist")))goto done;
 if(!keep_bg_jobs) {
  // Clear any staged OTA markers written by earlier boots so loginwindow never spawns Install-in-Progress at Shut Down.
  static const char *const ota_markers[]={
   "/private/var/db/softwareupdate/SoftwareUpdateAvailable.plist",
   "/private/var/db/softwareupdate/journal.plist",
   "/Library/Updates/index.plist",
   "/Library/Updates/ProductMetadata.plist",
   "/System/Volumes/Data/Library/Updates/index.plist",
   "/System/Volumes/Data/Library/Updates/ProductMetadata.plist",
  };
  for(unsigned oi=0;oi<sizeof(ota_markers)/sizeof(ota_markers[0]);++oi) {
   if(copyout(ota_markers[oi],scratch,strlen(ota_markers[oi])+1)==0) {
    struct unlink_args urm={.path=scratch};
    (void)unlink(p,&urm,&result);
   }
  }
 }
#endif
 done:
 // A real empty migration marker selects the qualified modern override loader.
 if(!error) {
  static const char *const markers[]={
   "/private/var/db/com.apple.xpc.launchd/disabled.migrated",
#if XNU_TARGET_OS_OSX
   "/private/var/db/com.apple.xpc.launchd/disabled.501.migrated",
   "/private/var/db/com.apple.xpc.launchd/disabled.502.migrated",
#endif
  };
  for(unsigned mi=0;mi<sizeof(markers)/sizeof(markers[0]) && !error;++mi) {
   error=copyout(markers[mi],scratch,strlen(markers[mi])+1);
   if(!error) {
    struct open_args mark={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
    error=open(p,&mark,&result);
    if(!error){struct close_args cl={.fd=result};error=sys_close(p,&cl,&result);}
    else if(error==EEXIST)error=0;
   }
  }
 }
 sheng_watchdog_policy_status=error;
#if XNU_TARGET_OS_OSX
 if(!keep_sep_jobs && !error)printf("sheng launch policy: disabled unavailable SEP/FairPlay jobs\n");
 if(!keep_bg_jobs && !error)printf("sheng launch policy: disabled missing-coprocessor/background CPU-hog jobs\n");
#endif
 if(lite==2 && !error)printf("sheng launch policy: disabled com.apple.iconservices.iconservicesagent\n");
 printf("sheng bring-up watchdogd launch policy: %d (hardware watchdog not implemented)\n",error);
 return error;
}
