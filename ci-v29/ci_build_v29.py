#!/usr/bin/env python3
"""Fast GitHub Actions xcode-27 builder for V29 macOS 27.3 (26C5035r) `NativeAdrenoMetal.bundle` + `Shader Cache.app`:
  - Reverts unmerged PR #66 (`EncodeBatch::copyBytes` + `adoptRenderServerThreadPolicy` + `NATIVE_METAL_FLOW=2`),
    eliminating the `0x06bf0c: blraaz x8` (`ESR_EC_FPAC`) `WindowServer` crash loop proven in `BootLog-25`.
  - Preserves 100% of V28's hardware-verified cures: persistent serialized 16 MiB `native-metal-pipeline` worker
    (`0` `iconservicesagent` crashes in `BootLog-25`!), restored `<USER_CACHE_DIR>/com.apple.metal/m2v` persistent
    shader disk cache, `0065` `bfloat->bool` (`#68`), and live + pre-primed VFX `applySystemTreatments` (`#70`).
  - Reuses the exact cached Rust 1.98.0 `libadreno_metal_bridge.a` (`--reuse-bridge`, runtime `c1014156186f5a1c72d0af030649956e`)
    and pairs offline with V28's already hardware-verified `kernel.macho` (`663d60e8…`).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

EXPECTED_TURNIP_UUID_HEX = "0688bc18ab163a08b00cfb68886015a6"
EXPECTED_V28_RUNTIME_ID = "c1014156186f5a1c72d0af030649956e"


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
        if cmd == 0x1B:
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
    import sys
    import tarfile

    ws = Path.cwd().resolve()
    ci_dir = ws / "repo/ci-v29"
    unixnu = ws / "UniXNU"
    sheng = ws / "UniXNU-sheng"
    sm8550 = ws / "Sm8550Platform-2dc650ca"
    shim_bin = ws / "shim-bin"
    stage_out = ws / "v29-stage-out"

    if not os.environ.get("ARTIFACT_KEY"):
        raise SystemExit("ARTIFACT_KEY secret is required before building")

    # 1. Neutral `whoami` == builder shim + Rust/LLVM/Homebrew PATH
    shim_bin.mkdir(parents=True, exist_ok=True)
    whoami_sh = shim_bin / "whoami"
    whoami_sh.write_text("#!/bin/sh\necho builder\n", encoding="utf-8", newline="\n")
    whoami_sh.chmod(0o755)

    extra_paths = [
        str(shim_bin),
        "/opt/homebrew/opt/rustup/bin",
        str(Path.home() / ".cargo/bin"),
        "/opt/homebrew/opt/python@3.13/libexec/bin",
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

    # 2. Apply pure-text V29 overlay + Sm8550Platform
    if sm8550.exists():
        shutil.rmtree(sm8550)
    shutil.copytree(ci_dir / "Sm8550Platform-2dc650ca", sm8550)

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
    print(f"Applied & verified {len(manifest['files'])} V29 overlay files", flush=True)

    # 3. Fast-fail host C11 & C++17 unit tests on macOS clang/clang++ (-Wall -Wextra -Werror)
    nvt_obj = ws / "NvtTouchCore_v29_test.o"
    bm_obj = ws / "ShengBattmgr_v29_test.o"
    unit_bin = ws / "v29_unit_test_bin"
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
            ci_dir / "v29_unit_test.cpp",
            nvt_obj,
            bm_obj,
            "-lm", "-o", unit_bin,
        ],
        env=env,
    )
    run([unit_bin], env=env)
    for tmp_b in (nvt_obj, bm_obj, unit_bin):
        tmp_b.unlink(missing_ok=True)

    # 4. Defconfig + assemble tree (preserves cached `tree/build/adreno-metal-bridge` & `tree/.cache`!)
    run(["python3", "./unixnu", "defconfig", "sheng_macos_defconfig", "-D", "../UniXNU-sheng", "--os", "macos-27"], cwd=unixnu, env=env)
    run(
        [
            "python3", "./unixnu", "tree",
            "-D", "../UniXNU-sheng", "--os", "macos-27",
            "--local-module", f"kit:Sm8550Platform={sm8550}",
        ],
        cwd=unixnu,
        env=env,
    )
    work_out = unixnu / "out/sheng/macos-27.3-26C5035r/sheng_macos"
    tree = work_out / "tree"

    # 5. Prepare Mesa Vulkan headers & metal2vulkan checkout, then build V29 NativeAdrenoMetal.bundle + Shader Cache.app
    sys.path.insert(0, str(tree / "scripts"))
    from mesa_patch_identity import validate_recorded_patches  # noqa: E402

    mesa_pins = json.loads((tree / "scripts/data/adreno-compiler-inputs.json").read_text(encoding="utf-8"))
    assert len(mesa_pins["patches"]) == 27
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

    run(
        [
            "python3", tree / "scripts/build-adreno-metal-plugin.py",
            "--target", "macos27", "--no-replay", "--reuse-bridge",
        ],
        cwd=tree,
        env=env,
    )

    plugin_out = tree / "build/adreno-metal-plugin-macos27"
    plugin_bundle = plugin_out / "NativeAdrenoMetal.bundle"
    plugin_exe = plugin_bundle / "Contents/MacOS/NativeAdrenoMetal"
    plugin_info = plugin_bundle / "Contents/Info.plist"
    plugin_gpubuild = plugin_bundle / "Contents/Resources/gpu-build.json"
    sc_app = plugin_out / "Shader Cache.app"

    assert plugin_exe.is_file() and plugin_info.is_file() and plugin_gpubuild.is_file() and sc_app.is_dir()
    praw = plugin_exe.read_bytes()

    # Verify V29 cured PR #66 symbols/strings are 100% GONE while V28 verified cures remain 100% PRESENT
    assert b"copyBytes" not in praw, "Broken PR #66 copyBytes still present in V29 NativeAdrenoMetal"
    assert b"adoptRenderServerThreadPolicy" not in praw, "PR #66 adoptRenderServerThreadPolicy still present"
    assert b"native-metal-pipeline\x00" in praw, "Missing V28 persistent serialized pipeline worker"
    assert b"vfx::api::applySystemTreatments\x00" in praw, "Missing PR #70 live VFX linker"
    assert b".nmb-probe" not in praw and b"com.apple.metal" in praw, "Missing V28 disk shader cache fix"
    assert b"@16@0:8\x00" in praw and b"Q16@0:8\x00" in praw and b"^v16@0:8\x00" in praw, "Missing V24 methodABI"
    assert b"NBVisibleFunctionTable\x00" in praw and b"NBDynamicLibrary\x00" in praw, "Missing V23 CoreImage VFT"
    assert b"Qualcomm Adreno 740\x00" in praw, "Missing Qualcomm Adreno 740 branding"

    gb_doc = json.loads(plugin_gpubuild.read_text(encoding="utf-8"))
    assert gb_doc.get("turnip_uuid") == EXPECTED_TURNIP_UUID_HEX
    assert gb_doc.get("runtime") == EXPECTED_V28_RUNTIME_ID, f"Expected runtime {EXPECTED_V28_RUNTIME_ID}, got {gb_doc}"
    assert gb_doc.get("cache_build") == f"{EXPECTED_V28_RUNTIME_ID}-{EXPECTED_TURNIP_UUID_HEX}"

    sc_worker_raw = (sc_app / "Contents/MacOS/ShaderCacheWorker").read_bytes()
    assert b"vfx::api::applySystemTreatments\x00" in sc_worker_raw

    if stage_out.exists():
        shutil.rmtree(stage_out)
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
        "schema": "sheng-ci-stage-v29-verify-v1",
        "sdk_version": sdk_ver,
        "rustc_version": rustc_ver,
        "shader_cache_trust": shader_trust,
        "gpu_build_json_ci": gb_doc,
        "files": sums,
    }
    (stage_out / "ci-verify.json").write_text(json.dumps(ci_verify, indent=2) + "\n", encoding="utf-8", newline="\n")

    tar_plain = ws / "v29-ci-stage.tar.gz"
    tar_enc = ws / "v29-ci-stage.tar.gz.enc"
    with tarfile.open(tar_plain, "w:gz") as tf:
        tf.add(stage_out, arcname="v29-stage")
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
        "plugin_exe_ci_sha256": sums["plugin/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal"]["sha256"],
        "metal_source_sha256": gb_doc["metal_source_sha256"],
        "cache_build": gb_doc["cache_build"],
    }
    (ws / "v29-ci-meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("=== V29 FAST PLUGIN CI BUILD & ENCRYPTED ARCHIVE COMPLETE ===", flush=True)
    print(json.dumps(meta, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
