#!/usr/bin/env python3
"""Fast XNU-only GitHub Actions xcode-27 builder for V31 macOS 27.3 (26C5035r),
derived 1:1 from proven `ci_build_v30.py` (`Run 45459425033`) with Rust/AdrenoMetal skipped:
  - Rebuilds `kernel.macho` + `kernel.macho.crypto-hmac.json` with `v31-addon-stamp-and-clean-fb.patch`
    (volatile MMIO-safe register-store GOP clear, eliminating `dc zva` Alignment Fault `ESR=0x96000061`).
  - Disassembles `_initialize_screen` in unstripped XNU Mach-O via `llvm-objdump` to verify ZERO `dc zva`
    instructions and `<= 1` (`vc_user_options` 32-byte) `_bzero` calls before encrypting!
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

EXPECTED_DEVAUTH_SHA = "33c6a607b159b824d6b65a61d6693c72afa39dbe7c2b344d574cb197f3750a04"
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


def main() -> int:
    ws = Path.cwd().resolve()
    ci_dir = ws / "repo/ci-v31"
    unixnu = ws / "UniXNU"
    sheng = ws / "UniXNU-sheng"
    sm8550 = ws / "Sm8550Platform-2dc650ca"
    uefi_font = ws / "uefi-font"
    shim_bin = ws / "shim-bin"
    stage_out = ws / "v31-stage-out"

    if not os.environ.get("ARTIFACT_KEY"):
        raise SystemExit("ARTIFACT_KEY secret is required before building")
    owner_uuid = os.environ.get("OWNER_DATA_VOLUME_UUID", "").strip().upper()
    if not re.fullmatch(r"[0-9A-F]{8}-(?:[0-9A-F]{4}-){3}[0-9A-F]{12}", owner_uuid):
        raise SystemExit("Valid OWNER_DATA_VOLUME_UUID secret is required before building")

    shim_bin.mkdir(parents=True, exist_ok=True)
    whoami_sh = shim_bin / "whoami"
    whoami_sh.write_text("#!/bin/sh\necho builder\n", encoding="utf-8", newline="\n")
    whoami_sh.chmod(0o755)

    extra_paths = [
        str(shim_bin),
        "/opt/homebrew/opt/python@3.13/libexec/bin",
        "/opt/homebrew/opt/bison/bin",
        "/opt/homebrew/opt/lld/bin",
        "/opt/homebrew/opt/llvm/bin",
        "/opt/homebrew/bin",
    ]
    env = dict(os.environ)
    env.pop("SDKROOT", None)
    env["PATH"] = ":".join(extra_paths + [env.get("PATH", "")])
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["ZERO_AR_DATE"] = "1"

    who = subprocess.check_output(["whoami"], env=env, text=True).strip()
    assert who == "builder", f"Expected whoami=='builder', got {who!r}"
    sdk_ver = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-version"], env=env, text=True).strip()
    print(f"Verified Xcode macosx SDK version: {sdk_ver}", flush=True)

    # 2. Apply pure-text V31 overlay + Sm8550Platform + uefi-font
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
    (unixnu / "unixnu").chmod(0o755)
    assert not (sheng / "patches/xnu-12377/v30-addon-stamp-and-clean-fb.patch").exists()
    assert (sheng / "patches/xnu-12377/v31-addon-stamp-and-clean-fb.patch").is_file()
    print(f"Applied & verified {len(manifest['files'])} V31 overlay files + {len(policy_blobs)} policy headers", flush=True)

    # 3b. Fetch SHA-256-pinned board firmware (Touch, Keyboard MCU_Upgrade.bin + devauth.mbn, Video)
    run(["python3", "scripts/extract-files.py"], cwd=sheng, env=env)
    devauth_src = sheng / ".cache/firmware/qcom/sm8550/sheng/devauth.mbn"
    assert devauth_src.is_file() and sha256_file(devauth_src) == EXPECTED_DEVAUTH_SHA

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
    assert "aks-sls=3" not in boot_args
    seal_env = dict(env, **ux_kernel.version_env({}, ctx))

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

    # 5. Build & prelink XNU + drivers
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

    prelinked_kernel = work_out / "kernel/kernel.macho"
    payload_kernel = stage_out / "out/kernel.macho"
    shutil.copyfile(prelinked_kernel, payload_kernel)

    probe_env = dict(env, UNIXNU_UEFI=str(uefi_font))
    run(
        ["python3", tree / "scripts/patch-xnu-entry-probe.py", payload_kernel, "--panic-screen", "--no-reset"],
        cwd=tree,
        env=probe_env,
    )
    run(
        ["python3", tree / "scripts/seal-source-crypto-kernel.py", payload_kernel, "--kernel-args", boot_args],
        cwd=tree,
        env=seal_env,
    )
    hmac_json = stage_out / "out/kernel.macho.crypto-hmac.json"
    assert hmac_json.is_file(), "Missing kernel.macho.crypto-hmac.json after sealing"

    # 6. Hard disassembly verification on _initialize_screen in unstripped Mach-O!
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

    kraw = payload_kernel.read_bytes()
    banners = [m.decode("ascii", "replace") for m in re.findall(rb"Darwin Kernel Version [^\x00\n]{10,160}", kraw)]
    assert any("; builder:ios-obj/DEVELOPMENT_ARM64_BCM2837" in b for b in banners), f"Missing neutral builder banner: {banners}"
    assert owner_uuid.encode("ascii") in kraw
    assert EXPECTED_EXEC_ENV_V30 in kraw
    assert b"BackgroundShortcutRunner,STExtractionService.privileged,softwareupdated,mobileassetd,Install-in-Progress,amsengagementd" in kraw
    assert b"/private/var/db/softwareupdate/SoftwareUpdateAvailable.plist\x00" in kraw
    assert b"sheng power button held >= 4.5s: triggering orderly kernel halt (RB_HALT)\n\x00" in kraw
    assert b"/private/var/ngpu/.addon-stamp\x00" in kraw
    assert b"gpu-delay-ms\x00" in kraw
    assert b"NativeSandboxGrantRead\x00" in kraw
    assert b"sheng touch: thermal baseline runaway recovered" in kraw

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
        "schema": "sheng-ci-stage-v31-verify-v1",
        "sdk_version": sdk_ver,
        "kernel_banners": banners,
        "boot_args": boot_args,
        "initialize_screen_bzero_calls": len(bzero_calls),
        "initialize_screen_zero_dc_zva": True,
        "files": sums,
    }
    (stage_out / "ci-verify.json").write_text(json.dumps(ci_verify, indent=2) + "\n", encoding="utf-8", newline="\n")

    tar_plain = ws / "v31-ci-stage.tar.gz"
    tar_enc = ws / "v31-ci-stage.tar.gz.enc"
    with tarfile.open(tar_plain, "w:gz") as tf:
        tf.add(stage_out, arcname="v31-stage")
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
        "initialize_screen_bzero_calls": len(bzero_calls),
        "initialize_screen_zero_dc_zva": True,
    }
    (ws / "v31-ci-meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("=== V31 FAST KERNEL CI BUILD & ENCRYPTED ARCHIVE COMPLETE ===", flush=True)
    print(json.dumps(meta, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
