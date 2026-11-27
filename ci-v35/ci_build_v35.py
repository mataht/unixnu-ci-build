#!/usr/bin/env python3
"""GitHub Actions xcode-27 builder for V35 macOS 27.3 (26C5035r) full stage:
  - Rebuilt `NativeAdrenoMetal.bundle`:
    * V35 Electron / Chromium / ANGLE Metal zero-copy tile & glyph coherency cure (fix false `stage == 2` `gpuWritten`
      on `MTLTextureUsageShaderWrite` `IOSurface`s, cross-process `flags = 16u` GPU-write notification, client `flags = 8u`
      & `NBBuffer` subrange `flags = 32u` `BoClean`, `MTLResourceStorageModeManaged -> Shared` unified-memory normalization,
      wired `didModifyRange:`, `NBRenderEncoder` `memoryBarrierWith*`/`textureBarrier`, `NBBlitEncoder` `synchronizeTexture:`,
      and `-std=macos-metal%u.%u` on `TARGET_OS_OSX`)
    * Preserved V34 scoped Chromium `MTLGPUFamilyMac1`/`Mac2`/`Common3` + `ANGLE_DEFAULT_PLATFORM=metal` + `.appex` fast skip
      + V33..V23 cures & warm `NMB_PREBUILT_BRIDGE`
  - Rebuilt `Shader Cache.app` (V33 `ShaderCacheRegister` `/private/var/ngpu/Applications/Shader Cache.app` `bindfs` `-36` cure)
  - Rebuilt `kernel.macho` + `kernel.macho.crypto-hmac.json`:
    * V35 `Adreno740` cross-process `ImportPageRef.gpu_written`, `importPersistent()` guard on `importedUntouched`, and
      page-aligned sub-range `cleanBo` (`flags` `1u/2u/4u/8u/16u/32u`)
    * Preserved V34 lock-free `BoImport` + `BoMap` PoC cache clean, contiguous spans, deferred `BoFree` unwire,
      `IOUserClientDefaultLocking = false`, soft-corpse skip, `CpuFreqProbe`/`ThermalCapPolicy` v5/`GpuPowerProbe` `401 MHz`,
      `120 Hz` `ReportInterval=8333`, and V33..V5 kernel cures (`0` `dc zva` verified by `llvm-objdump`)
  - Rebuilt `unixnu-absentd` (V33 bounded `5000 ms` hold for `assistant*`/`siri*`/`softwareupdated*` + `250 ms` default)
  - Verified `devauth.mbn`

Runs inside a public GitHub Actions workspace with zero private identifiers in Git.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

EXPECTED_DEVAUTH_SHA = "33c6a607b159b824d6b65a61d6693c72afa39dbe7c2b344d574cb197f3750a04"
EXPECTED_TURNIP_UUID_HEX = "0688bc18ab163a08b00cfb68886015a6"
EXPECTED_EXEC_ENV_V30 = (
    b"exec-env=iconservicesagent|iconservicesd|System_Settings|Finder|Dock|ControlCenter|SystemUIServer|"
    b"loginwindow|Terminal|com.apple.appkit.xpc.openAndSavePanelService|QuickLookUIService|@appex:FORCE_SOFTWARE_RENDERER=1"
)


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(4 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str | Path], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    str_cmd = [str(c) for c in cmd]
    print("==>", " ".join(str_cmd), flush=True)
    subprocess.run(str_cmd, cwd=str(cwd) if cwd else None, env=env, check=True)


def checkout_pinned(dest: Path, spec: dict[str, str], env: dict[str, str]) -> bool:
    commit = spec["commit"]
    url = spec.get("url") or spec.get("repository")
    assert url and commit, f"Bad pin spec: {spec}"
    cur = ""
    if (dest / ".git").exists():
        cur = subprocess.run(["git", "-C", str(dest), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if cur == commit:
        return False
    if dest.exists():
        shutil.rmtree(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "init", "-q", dest], env=env)
    run(["git", "-C", dest, "remote", "add", "origin", url], env=env)
    run(["git", "-C", dest, "fetch", "-q", "--depth=1", "--filter=blob:none", "origin", commit], env=env)
    run(["git", "-C", dest, "checkout", "-q", "--detach", "FETCH_HEAD"], env=env)
    return True


def build_turnip_uuid_stub(dst_dylib: Path, tree: Path, sdk_path: str, uuid_hex: str, env: dict[str, str]) -> None:
    dst_dylib.parent.mkdir(parents=True, exist_ok=True)
    stub_c = dst_dylib.parent / "_turnip_uuid_stub.c"
    stub_c.write_text(
        "#include <stdlib.h>\nvoid *_turnip_uuid_stub(void *p) { free(p); return (void *)&_turnip_uuid_stub; }\n",
        encoding="utf-8",
        newline="\n",
    )
    run(
        [
            "xcrun", "clang", "-target", "arm64e-apple-macos27.0",
            "-isysroot", sdk_path,
            "-dynamiclib", "-O2",
            stub_c, "-o", dst_dylib,
        ],
        env=env,
    )
    stub_c.unlink(missing_ok=True)

    raw = bytearray(dst_dylib.read_bytes())
    magic, _, _, _, ncmds = struct.unpack_from("<IIIII", raw, 0)
    assert magic == 0xFEEDFACF, f"Expected MH_MAGIC_64, got {hex(magic)}"
    off = 32
    patched = False
    target_uuid = bytes.fromhex(uuid_hex)
    assert len(target_uuid) == 16
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", raw, off)
        if cmd == 0x1B:  # LC_UUID
            assert cmdsize == 24
            raw[off + 8 : off + 24] = target_uuid
            patched = True
            break
        off += cmdsize
    assert patched, "Missing LC_UUID in stub dylib"
    dst_dylib.write_bytes(bytes(raw))
    run(["codesign", "--force", "--sign", "-", "--pagesize", "4096", dst_dylib], env=env)

    uuid_out = subprocess.check_output(["xcrun", "dwarfdump", "--uuid", str(dst_dylib)], env=env, text=True)
    got_m = re.search(r"UUID:\s+([0-9a-fA-F-]{36})", uuid_out)
    assert got_m and got_m.group(1).replace("-", "").lower() == uuid_hex, f"UUID mismatch: {uuid_out}"

    probe_copy = dst_dylib.parent / "_turnip_uuid_stub_probe.dylib"
    shutil.copyfile(dst_dylib, probe_copy)
    run(["strip", "-x", probe_copy], env=env)
    run(["python3", tree / "scripts/macho-self-bind-weak-imports.py", probe_copy], env=env)
    probe_copy.unlink(missing_ok=True)


def main() -> int:
    ws = Path.cwd().resolve()
    ci_dir = ws / "repo/ci-v35"
    unixnu = ws / "UniXNU"
    sheng = ws / "UniXNU-sheng"
    sm8550 = ws / "Sm8550Platform-2dc650ca"
    uefi_font = ws / "uefi-font"
    shim_bin = ws / "shim-bin"
    stage_out = ws / "v35-stage-out"

    if not os.environ.get("ARTIFACT_KEY"):
        raise SystemExit("ARTIFACT_KEY secret is required before building")
    owner_uuid = os.environ.get("OWNER_DATA_VOLUME_UUID", "").strip().upper()
    if not re.fullmatch(r"[0-9A-F]{8}-(?:[0-9A-F]{4}-){3}[0-9A-F]{12}", owner_uuid):
        raise SystemExit("Valid OWNER_DATA_VOLUME_UUID secret is required before building")

    # 1. Set up neutral `whoami` == builder shim + Rust/LLVM/Homebrew PATH
    shim_bin.mkdir(parents=True, exist_ok=True)
    whoami_sh = shim_bin / "whoami"
    whoami_sh.write_text("#!/bin/sh\necho builder\n", encoding="utf-8", newline="\n")
    whoami_sh.chmod(0o755)

    extra_paths = [
        str(shim_bin),
        "/opt/homebrew/opt/rustup/bin",
        str(Path.home() / ".cargo/bin"),
        "/opt/homebrew/opt/python@3.13/libexec/bin",
        "/opt/homebrew/opt/bison/bin",
        "/opt/homebrew/opt/lld/bin",
        "/opt/homebrew/opt/llvm/bin",
        "/opt/homebrew/opt/shaderc/bin",
        "/opt/homebrew/opt/spirv-tools/bin",
        "/opt/homebrew/opt/glslang/bin",
        "/opt/homebrew/bin",
    ]
    env = dict(os.environ)
    env.pop("SDKROOT", None)
    env.pop("RUSTUP_TOOLCHAIN", None)
    env["PATH"] = ":".join(extra_paths + [env.get("PATH", "")])
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["ZERO_AR_DATE"] = "1"

    who = subprocess.check_output(["whoami"], env=env, text=True).strip()
    assert who == "builder", f"Expected whoami=='builder', got {who!r}"
    sdk_ver = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-version"], env=env, text=True).strip()
    sdk_path = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], env=env, text=True).strip()
    rustc_ver = subprocess.check_output(["rustc", "+1.98.0", "--version"], env=env, text=True).strip()
    print(f"Verified Xcode macosx SDK version: {sdk_ver} | Rust: {rustc_ver}", flush=True)

    # 2. Apply pure-text V35 overlay + Sm8550Platform + uefi-font (runs AFTER submodule pinning)
    if sm8550.exists():
        shutil.rmtree(sm8550)
    shutil.copytree(ci_dir / "Sm8550Platform-2dc650ca", sm8550)
    if uefi_font.exists():
        shutil.rmtree(uefi_font)
    shutil.copytree(ci_dir / "uefi-font", uefi_font)

    manifest = json.loads((ci_dir / "overlay-manifest.json").read_text(encoding="utf-8"))
    for rel, exp_sha in sorted(manifest["files"].items()):
        src = ci_dir / "overlay" / rel
        dst = ws / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        raw = src.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == exp_sha, f"Overlay source sha mismatch: {rel}"
        dst.write_bytes(raw)
        if dst.suffix in (".sh", ".py"):
            dst.chmod(0o755)
    policy_blobs = manifest.get("policy_blobs_hex", {})
    for rel, item in sorted(policy_blobs.items()):
        raw = bytes.fromhex(item["hex"])
        assert hashlib.sha256(raw).hexdigest() == item["sha256"], f"Policy blob sha mismatch: {rel}"
        dst = ws / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(raw)
    print(f"Applied & verified {len(manifest['files'])} V35 overlay files + {len(policy_blobs)} policy headers", flush=True)

    # 3. Fast-fail host C11 & C++17 unit tests on macOS clang/clang++ (-Wall -Wextra -Werror)
    nvt_bin = ws / "nvt_touch_core_bin"
    eeh_bin = ws / "exec_env_host_bin"
    qtee_bin = ws / "qtee_host_bin"
    wn_bin = ws / "wn8030_host_bin"
    bm_bin = ws / "battmgr_host_bin"
    sb_bin = ws / "screen_blank_host_bin"
    nvt_obj = ws / "NvtTouchCore_v35_test.o"
    bm_obj = ws / "ShengBattmgr_v35_test.o"
    unit_bin = ws / "v35_unit_test_bin"

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", sheng / "drivers/ShengNovatekTouch",
            sheng / "drivers/ShengNovatekTouch/NvtTouchCore.c",
            sheng / "drivers/ShengNovatekTouch/Test/touch-core.c",
            "-lm", "-o", nvt_bin,
        ],
        env=env,
    )
    run([nvt_bin], env=env)

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            sheng / "package/tests/exec_env_host.c",
            "-o", eeh_bin,
        ],
        env=env,
    )
    run([eeh_bin], env=env)

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", sheng / "package",
            sheng / "package/tests/qcom_tee_wire_host.c",
            sheng / "package/Wn8030.c",
            "-o", qtee_bin,
        ],
        env=env,
    )
    run([qtee_bin], env=env)

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", sheng / "package",
            sheng / "package/Wn8030.c",
            sheng / "package/tests/wn8030_host.c",
            "-o", wn_bin,
        ],
        env=env,
    )
    run([wn_bin], env=env)

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", sheng / "package",
            sheng / "package/ShengBattmgr.c",
            sheng / "package/tests/battmgr_host.c",
            "-o", bm_bin,
        ],
        env=env,
    )
    run([bm_bin], env=env)

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            sheng / "package/tests/screen_blank_host.c",
            "-o", sb_bin,
        ],
        env=env,
    )
    run([sb_bin], env=env)

    run(["python3", unixnu / "drivers/AppleKeyStoreState/test_closed_boundary.py"], env=env)

    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", sheng / "drivers/ShengNovatekTouch",
            "-c", sheng / "drivers/ShengNovatekTouch/NvtTouchCore.c",
            "-o", nvt_obj,
        ],
        env=env,
    )
    run(
        [
            "xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", sheng / "package",
            "-c", sheng / "package/ShengBattmgr.c",
            "-o", bm_obj,
        ],
        env=env,
    )
    run(
        [
            "xcrun", "clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", sheng / "drivers/ShengNovatekTouch",
            "-I", sheng / "package",
            "-I", sheng / "drivers/ShengHidInput",
            ci_dir / "v35_unit_test.cpp",
            nvt_obj,
            bm_obj,
            "-lm", "-o", unit_bin,
        ],
        env=env,
    )
    run([unit_bin], env=env)
    for tmp_b in (nvt_bin, eeh_bin, qtee_bin, wn_bin, bm_bin, sb_bin, nvt_obj, bm_obj, unit_bin):
        tmp_b.unlink(missing_ok=True)

    # 3b. Fetch SHA-256-pinned board firmware (Touch, Keyboard MCU_Upgrade.bin + devauth.mbn, Video)
    run(["python3", "scripts/extract-files.py"], cwd=sheng, env=env)
    devauth_src = sheng / ".cache/firmware/qcom/sm8550/sheng/devauth.mbn"
    assert devauth_src.is_file() and sha256_file(devauth_src) == EXPECTED_DEVAUTH_SHA, (
        f"Missing or mismatched devauth.mbn at {devauth_src}"
    )

    # 3c. Pre-stage & verify Ath12kWifi, QcomIris, and QcaBluetooth locked upstream sources
    wifi_dir = sheng / "drivers/Ath12kWifi"
    wifi_lock = json.loads((wifi_dir / "tools/source-lock.json").read_text(encoding="utf-8"))
    (wifi_dir / "deps").mkdir(parents=True, exist_ok=True)
    wifi_mirrors = {
        "linux": "https://github.com/gregkh/linux.git",
        "freebsd": "https://github.com/freebsd/freebsd-src.git",
    }
    for dep_name, mirror_url in wifi_mirrors.items():
        dep_spec = wifi_lock[dep_name]
        dep_dir = wifi_dir / "deps" / dep_name
        cur_head = ""
        if (dep_dir / ".git").exists():
            cur_head = subprocess.run(["git", "-C", str(dep_dir), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if cur_head != dep_spec["commit"]:
            if dep_dir.exists():
                shutil.rmtree(dep_dir)
            run(["git", "init", "-q", dep_dir], env=env)
            run(["git", "-C", dep_dir, "remote", "add", "origin", mirror_url], env=env)
            run(["git", "-C", dep_dir, "sparse-checkout", "set", "--no-cone", *dep_spec["paths"]], env=env)
            run(["git", "-C", dep_dir, "fetch", "-q", "--depth=1", "--filter=blob:none", "origin", dep_spec["commit"]], env=env)
            run(["git", "-C", dep_dir, "checkout", "-q", "--detach", "FETCH_HEAD"], env=env)
    run(["bash", "tools/fetch-sources.sh"], cwd=wifi_dir, env=env)
    run(["bash", "tools/prepare.sh"], cwd=wifi_dir, env=env)

    iris_dir = sheng / "drivers/QcomIris"
    iris_lock = json.loads((iris_dir / "tools/source-lock.json").read_text(encoding="utf-8"))["linux"]
    for rel_f, exp_digest in iris_lock["files"].items():
        dst_f = iris_dir / "deps/linux" / rel_f
        if dst_f.is_file() and sha256_file(dst_f) == exp_digest:
            continue
        raw_url = f"https://raw.githubusercontent.com/gregkh/linux/{iris_lock['commit']}/{rel_f}"
        req = urllib.request.Request(raw_url, headers={"User-Agent": "unixnu-fetch/1"})
        with urllib.request.urlopen(req, timeout=60) as resp:
            f_bytes = resp.read()
        assert hashlib.sha256(f_bytes).hexdigest() == exp_digest, f"QcomIris sha mismatch: {rel_f}"
        dst_f.parent.mkdir(parents=True, exist_ok=True)
        dst_f.write_bytes(f_bytes)
    run(["python3", "tools/fetch-sources.py", "--offline"], cwd=iris_dir, env=env)
    run(["python3", "tools/prepare-linux.py"], cwd=sheng / "drivers/QcaBluetooth", env=env)

    # 4. Defconfig + enforce Data volume UUID from secret + pre-validate payload boot_args + tree
    run(["python3", "./unixnu", "defconfig", "sheng_macos_defconfig", "-D", "../UniXNU-sheng", "--os", "macos-27"], cwd=unixnu, env=env)
    work_out = unixnu / "out/sheng/macos-27.3-26C5035r/sheng_macos"
    cfg_path = work_out / ".config"
    cfg_txt = cfg_path.read_text(encoding="utf-8")
    target_uuid = f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{owner_uuid}"'
    if "CONFIG_XNU_OWNER_DATA_VOLUME_UUID=" in cfg_txt:
        cfg_txt = re.sub(r"^CONFIG_XNU_OWNER_DATA_VOLUME_UUID=.*$", target_uuid, cfg_txt, flags=re.M)
    else:
        cfg_txt += "\n" + target_uuid + "\n"
    cfg_path.write_text(cfg_txt, encoding="utf-8", newline="\n")
    assert target_uuid in cfg_path.read_text(encoding="utf-8")

    sys.path.insert(0, str(unixnu / "scripts"))
    from unixnu import cli as ux_cli, kernel as ux_kernel, payload as ux_payload  # noqa: E402

    ux_args = ux_cli.parser().parse_args(
        ["build", "payload", "--os", "macos-27", "-D", str(sheng), "--local-module", f"kit:Sm8550Platform={sm8550}"]
    )
    ctx = ux_cli.Context(ux_args)
    spec = ctx.board["payload"]
    line = ux_payload.check_args(list(spec.get("kernel_args", [])))
    boot_args = " ".join([line, *spec.get("install", {}).get("confirm_args", [])])
    assert "aks-sls=3" not in boot_args, f"aks-sls=3 still present in macos-27 boot_args: {boot_args}"
    seal_env = dict(env, **ux_kernel.version_env({}, ctx))
    print(f"Pre-validated payload boot_args ({len(boot_args)} B)", flush=True)

    run(
        [
            "python3", "./unixnu", "fetch", "IOHIDFamily", "IOStorageFamily",
            "-D", "../UniXNU-sheng", "--os", "macos-27",
            "--local-module", f"kit:Sm8550Platform={sm8550}",
        ],
        cwd=unixnu,
        env=env,
    )
    run(
        [
            "python3", "./unixnu", "tree",
            "-D", "../UniXNU-sheng", "--os", "macos-27",
            "--local-module", f"kit:Sm8550Platform={sm8550}",
        ],
        cwd=unixnu,
        env=env,
    )
    tree = work_out / "tree"

    if stage_out.exists():
        shutil.rmtree(stage_out)
    (stage_out / "out").mkdir(parents=True, exist_ok=True)
    devauth_dst = stage_out / "firmware/qcom/sm8550/sheng/devauth.mbn"
    devauth_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(devauth_src, devauth_dst)

    # 5a. Fast-fail userspace build: unixnu-absentd (V33 5000ms assistant hold + 250ms default + V30 fast-exit)
    absentd_stage = stage_out / "absentd"
    run(["python3", tree / "scripts/absent-service/build.py", absentd_stage, "macos27"], cwd=tree, env=env)
    absentd_bin = absentd_stage / "bin/unixnu-absentd"
    assert absentd_bin.is_file()
    araw = absentd_bin.read_bytes()
    assert b"-shutdownstall\x00" in araw, "Missing -shutdownstall in unixnu-absentd"
    assert b"Install-in-Progress\x00" in araw, "Missing V30 Install-in-Progress fast exit in unixnu-absentd"
    assert b".app/Contents/MacOS/\x00" in araw and b".xpc/Contents/MacOS/\x00" in araw, "Missing .app/.xpc fast exit in unixnu-absentd"
    assert b"dispatch_after" in araw and b"com.apple.assistant" in araw, "Missing V33 5000ms assistant hold in unixnu-absentd"
    assert b"/System/Library/xpc/launchd.plist" in araw, "Missing compiled launchd.plist lookup in unixnu-absentd"
    assert b"com.apple.lskdd" in araw and b"com.apple.fairplayd" in araw, "Missing lskdd/fairplayd in unixnu-absentd"
    assert b"com.apple.libquitd" in araw and b"com.apple.spinreporterd" in araw, "Missing libquitd/spinreporterd in unixnu-absentd"

    # 5b. Build V35 `NativeAdrenoMetal.bundle` + `Shader Cache.app`
    sys.path.insert(0, str(tree / "scripts"))
    from mesa_patch_identity import validate_recorded_patches  # noqa: E402

    mesa_pins = json.loads((tree / "scripts/data/adreno-compiler-inputs.json").read_text(encoding="utf-8"))
    assert len(mesa_pins["patches"]) == 27, f"Expected 27 mesa patches, got {len(mesa_pins['patches'])}"
    mesa_dir = tree / ".cache/mesa-work"
    fresh_mesa = checkout_pinned(mesa_dir, mesa_pins["mesa"], env)
    if fresh_mesa:
        for p_rel in mesa_pins["patches"]:
            run(["git", "-C", mesa_dir, "apply", tree / p_rel], env=env)
    else:
        try:
            validate_recorded_patches(tree, mesa_dir, mesa_pins)
        except Exception:
            run(["git", "-C", mesa_dir, "checkout", "-q", "--detach", "-f", mesa_pins["mesa"]["commit"]], env=env)
            run(["git", "-C", mesa_dir, "clean", "-fdx", "-q"], env=env)
            for p_rel in mesa_pins["patches"]:
                run(["git", "-C", mesa_dir, "apply", tree / p_rel], env=env)
    validate_recorded_patches(tree, mesa_dir, mesa_pins)

    m2v_pin = json.loads((tree / "scripts/data/gpu-metal-vulkan-inputs.json").read_text(encoding="utf-8"))["translator"]
    assert m2v_pin["commit"] == "46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71"
    m2v_dir = tree / ".cache/gpu-reference/metal2vulkan"
    checkout_pinned(m2v_dir, m2v_pin, env)
    run(["git", "-C", m2v_dir, "checkout", "-q", "--detach", "-f", m2v_pin["commit"]], env=env)
    run(["git", "-C", m2v_dir, "clean", "-fdx", "-q"], env=env)

    stub_turnip = tree / "build/mesa-tu-macos27/src/freedreno/vulkan/libvulkan_freedreno.dylib"
    build_turnip_uuid_stub(stub_turnip, tree, sdk_path, EXPECTED_TURNIP_UUID_HEX, env)

    plugin_env = dict(env)
    prebuilt_meta = tree / "build/adreno-metal-bridge/macos27/libadreno_metal_bridge.a.json"
    prebuilt_arch = tree / "build/adreno-metal-bridge/macos27/libadreno_metal_bridge.a"
    if prebuilt_meta.is_file() and prebuilt_arch.is_file():
        plugin_env["NMB_PREBUILT_BRIDGE"] = str(prebuilt_meta)
        print(f"Found cached Rust bridge archive ({prebuilt_arch.stat().st_size} B); verifying identity via NMB_PREBUILT_BRIDGE", flush=True)

    run(["python3", tree / "scripts/build-adreno-metal-plugin.py", "--target", "macos27", "--no-replay"], cwd=tree, env=plugin_env)

    plugin_out = tree / "build/adreno-metal-plugin-macos27"
    plugin_bundle = plugin_out / "NativeAdrenoMetal.bundle"
    plugin_exe = plugin_bundle / "Contents/MacOS/NativeAdrenoMetal"
    plugin_info = plugin_bundle / "Contents/Info.plist"
    plugin_gpubuild = plugin_bundle / "Contents/Resources/gpu-build.json"
    sc_app = plugin_out / "Shader Cache.app"

    assert plugin_exe.is_file() and plugin_info.is_file() and plugin_gpubuild.is_file() and sc_app.is_dir()
    praw = plugin_exe.read_bytes()
    # V35 checks
    assert b"-std=macos-metal%u.%u\x00" in praw, "Missing V35 -std=macos-metal%u.%u in NativeAdrenoMetal"
    assert b"gpu-keep-managed=1\x00" in praw, "Missing V35 StorageModeManaged->Shared normalization in NativeAdrenoMetal"
    assert b"memoryBarrierWithScope:afterStages:beforeStages:\x00" in praw, "Missing V35 NBRenderEncoder memoryBarrierWithScope in NativeAdrenoMetal"
    assert b"memoryBarrierWithResources:count:afterStages:beforeStages:\x00" in praw, "Missing V35 NBRenderEncoder memoryBarrierWithResources in NativeAdrenoMetal"
    assert b"synchronizeTexture:slice:level:\x00" in praw, "Missing V35 NBBlitEncoder synchronizeTexture in NativeAdrenoMetal"
    assert b"gpuWriteNotified\x00" in praw and b"hostImportState\x00" in praw, "Missing V35 gpuWriteNotified/hostImportState in NativeAdrenoMetal"
    # Preserved V34..V23 checks
    assert b"gpu-chromium-mac2=0\x00" in praw, "Missing V34 scoped Chromium Mac2 flag in NativeAdrenoMetal"
    assert b"ANGLE_DEFAULT_PLATFORM\x00" in praw, "Missing V34 ANGLE_DEFAULT_PLATFORM=metal in NativeAdrenoMetal"
    assert b".appex/Contents/MacOS/\x00" in praw and b"appex-metal=1\x00" in praw, "Missing V34 .appex fast skip in NativeAdrenoMetal"
    assert b"maxComputeThreadgroupMemory\x00" in praw, "Missing V34 installAnswers clean fallbacks in NativeAdrenoMetal"
    assert b"iosurfaceTextureAlignmentBytes\x00" in praw, "Missing V32 16B iosurfaceTextureAlignmentBytes in NativeAdrenoMetal"
    assert b"iosurfaceReadOnlyTextureAlignmentBytes\x00" in praw, "Missing iosurfaceReadOnlyTextureAlignmentBytes in NativeAdrenoMetal"
    assert b"gpu-prime-system=0\x00" in praw, "Missing V32 default-on system shader cache in NativeAdrenoMetal"
    assert b"file-write-create\x00" in praw, "Missing V32 silent sandbox_check in NativeAdrenoMetal"
    assert b"native-metal-pipeline\x00" in praw, "Missing persistent serialized pipeline worker in NativeAdrenoMetal"
    assert b"vfx::api::applySystemTreatments\x00" in praw, "Missing live VFX applySystemTreatments linker in NativeAdrenoMetal"
    assert b"iconservices-mps=1\x00" in praw, "Missing V30 iconservicesagent CI_ENABLE_MPS=0 cure in NativeAdrenoMetal"
    assert b"nb_newFunctionForPriming:requiredOnly:oneIndex:\x00" in praw, "Missing PR #69 priming selector in NativeAdrenoMetal"
    assert b"copyBytes" not in praw, "Broken PR #66 copyBytes must NEVER be present in NativeAdrenoMetal!"
    assert b"adoptRenderServerThreadPolicy" not in praw, "Broken PR #66 adoptRenderServerThreadPolicy must NOT be present"
    assert b"@16@0:8\x00" in praw and b"Q16@0:8\x00" in praw and b"^v16@0:8\x00" in praw, "Missing V24 methodABI framed encodings"
    assert b"NBVisibleFunctionTable\x00" in praw and b"NBDynamicLibrary\x00" in praw, "Missing V23 CoreImage VFT/DynamicLibrary"
    assert b".nmb-probe" not in praw, "Stale V23 .nmb-probe still present in NativeAdrenoMetal"
    assert b"Qualcomm Adreno 740\x00" in praw, "Missing Qualcomm Adreno 740 branding in NativeAdrenoMetal"

    gb_doc = json.loads(plugin_gpubuild.read_text(encoding="utf-8"))
    assert gb_doc.get("turnip_uuid") == EXPECTED_TURNIP_UUID_HEX, f"Unexpected turnip_uuid in gpu-build.json: {gb_doc}"

    sc_reg_raw = (sc_app / "Contents/MacOS/ShaderCacheRegister").read_bytes()
    assert b"/private/var/ngpu/Applications/Shader Cache.app" in sc_reg_raw, (
        "Missing V33 /private/var/ngpu/Applications/Shader Cache.app in ShaderCacheRegister"
    )
    sc_worker_raw = (sc_app / "Contents/MacOS/ShaderCacheWorker").read_bytes()
    assert b"vfx::api::applySystemTreatments\x00" in sc_worker_raw, "Missing PR #70 VFX linker in ShaderCacheWorker"
    assert b"InvalidDescriptor(OverlappingArgument" in sc_worker_raw, "Missing PR #69 primeCompute in ShaderCacheWorker"
    assert b" inputs=4 boot=" in sc_worker_raw, "Missing PR #69 inputs=4 log marker in ShaderCacheWorker"

    plugin_stage = stage_out / "plugin"
    for rel_p in (
        "NativeAdrenoMetal.bundle/Contents/Info.plist",
        "NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal",
        "NativeAdrenoMetal.bundle/Contents/Resources/gpu-build.json",
        "Shader Cache.app/Contents/Info.plist",
        "Shader Cache.app/Contents/MacOS/ShaderCache",
        "Shader Cache.app/Contents/MacOS/ShaderCacheRegister",
        "Shader Cache.app/Contents/MacOS/ShaderCacheWorker",
        "Shader Cache.app/Contents/_CodeSignature/CodeResources",
    ):
        src_p = plugin_out / rel_p
        dst_p = plugin_stage / rel_p
        dst_p.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src_p, dst_p)

    shader_trust = json.loads((plugin_out / "shader-cache-trust.json").read_text(encoding="utf-8"))

    # 6. Build & prelink XNU + drivers
    run(
        [
            "python3", "./unixnu", "build", "kernel",
            "-D", "../UniXNU-sheng",
            "--os", "macos-27",
            "--local-module", f"kit:Sm8550Platform={sm8550}",
        ],
        cwd=unixnu,
        env=env,
    )

    # 7. Patch panic screen / entry probe + seal source corecrypto POST HMAC
    prelinked_kernel = work_out / "kernel/kernel.macho"
    payload_kernel = stage_out / "out/kernel.macho"
    shutil.copyfile(prelinked_kernel, payload_kernel)

    probe_env = dict(env, UNIXNU_UEFI=str(uefi_font))
    run(
        ["python3", tree / "scripts/patch-xnu-entry-probe.py", payload_kernel, "--panic-screen", "--no-reset"],
        cwd=tree,
        env=probe_env,
    )

    print(f"Sealing kernel.macho with boot_args ({len(boot_args)} B)", flush=True)
    run(
        ["python3", tree / "scripts/seal-source-crypto-kernel.py", payload_kernel, "--kernel-args", boot_args],
        cwd=tree,
        env=seal_env,
    )
    hmac_json = stage_out / "out/kernel.macho.crypto-hmac.json"
    assert hmac_json.is_file(), "Missing kernel.macho.crypto-hmac.json after sealing"

    # 8. Hard disassembly verification on _initialize_screen + binary feature checks
    unstripped = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf"
    disasm = subprocess.check_output(
        ["llvm-objdump", "--disassemble-symbols=_initialize_screen", str(unstripped)],
        env=env,
        text=True,
    )
    assert "<_initialize_screen>:" in disasm, "Failed to disassemble _initialize_screen"
    assert "dc\tzva" not in disasm and "dc zva" not in disasm, f"FATAL: dc zva present in _initialize_screen:\n{disasm}"
    bzero_calls = [ln for ln in disasm.splitlines() if "_bzero" in ln or "_memset" in ln]
    assert len(bzero_calls) <= 1, f"FATAL: unexpected _bzero/_memset call in _initialize_screen: {bzero_calls}"
    print(f"Verified _initialize_screen disassembly: 0 dc zva, bzero_calls={len(bzero_calls)}", flush=True)

    corpse_disasm = subprocess.check_output(
        ["llvm-objdump", "--disassemble-symbols=_PROC_CROSSED_HIGH_WATERMARK__SEND_EXC_RESOURCE_AND_SUSPEND", str(unstripped)],
        env=env,
        text=True,
    )
    assert "<_PROC_CROSSED_HIGH_WATERMARK__SEND_EXC_RESOURCE_AND_SUSPEND>:" in corpse_disasm

    kraw = payload_kernel.read_bytes()
    banners = [m.decode("ascii", "replace") for m in re.findall(rb"Darwin Kernel Version [^\x00\n]{10,160}", kraw)]
    assert any("; builder:ios-obj/DEVELOPMENT_ARM64_BCM2837" in b for b in banners), f"Missing neutral builder banner: {banners}"
    assert owner_uuid.encode("ascii") in kraw, "Missing owner Data volume UUID in kernel.macho"

    # V35 + V34 + V33 + V32 kernel checks
    assert b"IOUserClientDefaultLocking\x00" in kraw, "Missing V34 IOUserClientDefaultLocking in kernel.macho"
    assert b"vendor-id\x00" in kraw, "Missing V34 vendor-id in kernel.macho"
    assert b"ReportInterval\x00" in kraw, "Missing V34 120Hz ReportInterval in kernel.macho"
    assert b"assistantd,assistant_service,audiomxd" in kraw, "Missing V33 assistantd/audiomxd deny_exec in kernel.macho"
    assert b"com.apple.audiomxd" in kraw and b"com.apple.Siri.agent,com.apple.assistant_service,com.apple.assistantd" in kraw, (
        "Missing V33 BootWatchdogPolicy audiomxd/Siri disables"
    )
    assert b"StateOfHealth\x00" in kraw and b"MaxCapacity\x00" in kraw, "Missing V33 StateOfHealth/MaxCapacity in kernel.macho"
    assert b"/private/var/ngpu/shader-cache/" in kraw, "Missing V33 shader-cache VFS grant in kernel.macho"
    assert b"IOMFBSupportsICC\x00" in kraw and b"DPTimingModeId\x00" in kraw, "Missing V32 IOMFBSupportsICC=false in kernel.macho"
    assert b"SoftwareUpdateNotificationManager,assetsubscriptiond" in kraw, "Missing V32 SU/assetsubscriptiond deny_exec in kernel.macho"
    assert b"UNX-SHENG-BAT0\x00" in kraw and b"NotChargingReason\x00" in kraw and b"FccComp1\x00" in kraw
    assert b"vm.footprint_suspend\x00" in kraw and b"wifip2pd\x00" in kraw
    assert b"/private/var/tmp/NativeAdrenoMetal.cache/" in kraw

    # Preserved V31..V22 kernel checks
    assert b"sheng touch: thermal baseline runaway recovered" in kraw
    assert EXPECTED_EXEC_ENV_V30 in kraw
    assert b"Weather|Stocks|Clock|Home" not in kraw and b"AboutExtension\x00" not in kraw
    assert b"acm_fastsim=1 gpu-pool=0 clpc=1 smc=1 cdsp=0 log-video=1" in kraw
    assert b"/private/var/db/com.apple.xpc.launchd/disabled.501.migrated\x00" in kraw
    assert b"/private/var/db/softwareupdate/SoftwareUpdateAvailable.plist\x00" in kraw
    assert b"sheng power button held >= 4.5s: triggering orderly kernel halt (RB_HALT)\n\x00" in kraw
    assert b"/private/var/ngpu/.addon-stamp\x00" in kraw and b"gpu-delay-ms\x00" in kraw
    assert b"NativeSandboxGrantRead\x00" in kraw and b"Touchscreen Pointer\x00" in kraw
    assert b"PointerAccelerationSupport\x00" not in kraw and b"sheng_touch_mode" not in kraw
    assert b"/private/var/ngpu/firmware/qcom/sm8550/sheng/devauth.mbn\x00" in kraw
    assert b"KA st=%d err=%d q=%x ta=%x" in kraw
    assert b"com.apple.driver.AppleCredentialManager" in kraw
    assert b"backboardd,WindowServer,corebrightnessd\x00" in kraw
    assert b"force-brightness-control\x00" in kraw and b"AppleRawExternalConnected\x00" in kraw
    assert b"panic-on-log-stall" in kraw and b"scrubbed unroutable ULA-only IPv6 RA" in kraw

    nm_out = subprocess.check_output(["llvm-nm", str(unstripped)], env=env, text=True)
    assert "native_sandbox_read_grant" in nm_out
    assert "_native_union_covered_volname" in nm_out
    assert "native_launchd_override_vnode" in nm_out
    assert "_proc_find_internal" in nm_out or "_proc_ref_nowait" in nm_out

    prelinked_nm = subprocess.check_output(["llvm-nm", str(payload_kernel)], env=env, text=True)
    assert "_nvt_touch_rebaseline" in prelinked_nm
    assert "_native_touch_abort_seq" in prelinked_nm
    assert "_native_keyboard_last_input_abs" in prelinked_nm
    assert "_sheng_devauth_offline_token" in prelinked_nm
    assert "_native_ufs_rpmb_scsi" in prelinked_nm
    assert "_native_hid_touch_arm_sink" in prelinked_nm
    assert "prepareImport" in prelinked_nm and "adoptImport" in prelinked_nm
    assert "markImportGpuWritten" in prelinked_nm and "importGpuWritten" in prelinked_nm

    absentd_cdhash = re.search(
        r"^CDHash=([0-9a-f]{40})$",
        subprocess.run(["codesign", "--display", "--verbose=4", str(absentd_bin)], capture_output=True, text=True, check=True).stderr,
        re.M,
    )[1]

    files_to_hash = sorted(p for p in stage_out.rglob("*") if p.is_file())
    sums = {}
    sums_lines = []
    for fp in files_to_hash:
        rel = str(fp.relative_to(stage_out)).replace("\\", "/")
        d = sha256_file(fp)
        sums[rel] = {"size": fp.stat().st_size, "sha256": d}
        sums_lines.append(f"{d}  {rel}")
    (stage_out / "SHA256SUMS").write_text("\n".join(sums_lines) + "\n", encoding="utf-8", newline="\n")

    ci_verify = {
        "schema": "sheng-ci-stage-v35-verify-v1",
        "sdk_version": sdk_ver,
        "rustc_version": rustc_ver,
        "kernel_banners": banners,
        "boot_args": boot_args,
        "initialize_screen_zero_dc_zva": True,
        "initialize_screen_bzero_calls": len(bzero_calls),
        "absentd_cdhash": absentd_cdhash,
        "shader_cache_trust": shader_trust,
        "gpu_build_json_ci": gb_doc,
        "files": sums,
    }
    (stage_out / "ci-verify.json").write_text(json.dumps(ci_verify, indent=2) + "\n", encoding="utf-8", newline="\n")

    tar_plain = ws / "v35-ci-stage.tar.gz"
    tar_enc = ws / "v35-ci-stage.tar.gz.enc"
    with tarfile.open(tar_plain, "w:gz") as tf:
        tf.add(stage_out, arcname="v35-stage")
    plain_sha = sha256_file(tar_plain)
    plain_sz = tar_plain.stat().st_size

    run(
        [
            "openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-salt",
            "-pass", "env:ARTIFACT_KEY",
            "-in", tar_plain,
            "-out", tar_enc,
        ],
        env=env,
    )
    tar_plain.unlink()
    enc_sha = sha256_file(tar_enc)
    meta = {
        "plain_tar_bytes": plain_sz,
        "plain_tar_sha256": plain_sha,
        "enc_tar_bytes": tar_enc.stat().st_size,
        "enc_tar_sha256": enc_sha,
        "kernel_sha256": sums["out/kernel.macho"]["sha256"],
        "hmac_sha256": sums["out/kernel.macho.crypto-hmac.json"]["sha256"],
        "plugin_exe_ci_sha256": sums["plugin/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal"]["sha256"],
        "shader_cache_register_ci_sha256": sums["plugin/Shader Cache.app/Contents/MacOS/ShaderCacheRegister"]["sha256"],
        "absentd_sha256": sums["absentd/bin/unixnu-absentd"]["sha256"],
        "absentd_cdhash": absentd_cdhash,
        "devauth_sha256": sums["firmware/qcom/sm8550/sheng/devauth.mbn"]["sha256"],
        "initialize_screen_zero_dc_zva": True,
        "initialize_screen_bzero_calls": len(bzero_calls),
    }
    (ws / "v35-ci-meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("=== V35 CI BUILD & ENCRYPTED ARCHIVE COMPLETE ===", flush=True)
    print(json.dumps(meta, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
