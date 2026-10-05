#!/usr/bin/env python3
"""Refresh CMR-AL09 camera, graphics and GNSS blobs from the C00 capture.

The stock archive is read only. Run without --apply to verify and preview.
This deliberately leaves radio and unrelated common blobs alone.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from io import BytesIO


DEVICE = Path(__file__).resolve().parent
ANDROID = DEVICE.parents[2]
COMMON = ANDROID / "vendor/huawei/hi3660/proprietary"
LIST = ANDROID / "device/huawei/hi3660/proprietary-files.txt"
PATCHELF = ANDROID / "prebuilts/extract-tools/linux-x86/bin/patchelf-0_17_2"
SECTIONS = {
    "Audio", "Bluetooth", "Camera", "Cameron hwcam", "Connectivity",
    "Fingerprint", "Firmware", "Cameron firmware", "Gatekeeper",
    "Graphics", "GPS/GNSS", "Keystore", "Media OMX", "Sensors",
    "TEE (vendor)", "TEE Configs", "Thermal",
}
EXTRA = (
    "vendor/firmware/ivp/*",
    "vendor/firmware/isp_bw.elf",
    "vendor/firmware/isp_dts.img",
    "vendor/firmware/isp_fw.elf",
)
SUPL_SOURCE = "system/app/gnss_supl20service_hisi/gnss_supl20service_hisi.apk"
SUPL_VDEX = "system/app/gnss_supl20service_hisi/oat/arm64/gnss_supl20service_hisi.vdex"
SUPL_TARGET = ANDROID / "vendor/huawei/hi3660/system/priv-app/gnss_supl20service_hisi/gnss_supl20service_hisi.apk"
VDEX_EXTRACTOR = ANDROID / "prebuilts/extract-tools/linux-x86/bin/vdexExtractor"
COMPACT_DEX_CONVERTER = ANDROID / "prebuilts/extract-tools/linux-x86/bin/compact_dex_converter"
DEXDUMP = ANDROID / "prebuilts/sdk/tools/linux/bin/dexdump"
MEDIA_SOURCE = "odm/etc/media_profiles_V1_0.xml"
MEDIA_TARGET = ANDROID / "device/huawei/hi3660/prebuilts/media_profiles_V1_0.xml"
RETIRED = (
    "vendor/bin/hw/vendor.huawei.hardware.gnss@1.0-service",
    "vendor/etc/init/vendor.huawei.hardware.gnss@1.0-service.rc",
    "vendor/lib64/hw/vendor.huawei.hardware.gnss@1.0-impl.so",
    "vendor/lib64/vendor.huawei.hardware.gnss@2.0.so",
    "vendor/bin/hw/vendor.huawei.hardware.graphics.displayeffect@1.2-service",
    "vendor/etc/init/vendor.huawei.graphics.displayeffect@1.2-service.rc",
    "vendor/lib/hw/vendor.huawei.hardware.graphics.displayeffect@1.2-impl.so",
    "vendor/lib64/hw/vendor.huawei.hardware.graphics.displayeffect@1.2-impl.so",
    "vendor/bin/hw/vendor.huawei.hardware.graphics.mediacomm@2.1-service",
    "vendor/etc/init/vendor.huawei.hardware.graphics.mediacomm@2.1-service.rc",
    "vendor/lib/hw/vendor.huawei.hardware.graphics.mediacomm@2.1-impl.so",
    "vendor/lib64/hw/vendor.huawei.hardware.graphics.mediacomm@2.1-impl.so",
    "vendor/lib64/vendor.huawei.hardware.camera.camResource.matcherService@1.0.so",
    "vendor/lib64/vendor.huawei.hardware.camera.camResource@1.4.so",
    "vendor/lib64/libRGBW.so",
)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def camera_config(name):
    return name.startswith(("vendor/etc/camera/", "odm/etc/camera/"))


def compatible_camera_config(data):
    for old, new in ((b"gb2312", b"iso-8859-1"),
                     (b"GB2312", b"iso-8859-1"),
                     (b"xmlversion", b"xml version")):
        data = data.replace(old, new)
    return data


def selections(stock):
    section = None
    specs = []
    for raw in LIST.read_text().splitlines():
        line = raw.strip()
        if line.startswith("# "):
            section = line[2:]
        elif line and not line.startswith("#") and section in SECTIONS:
            specs.append(line)
    specs.extend(EXTRA)
    result = {}
    for spec in specs:
        src, sep, dst = spec.lstrip("/").partition(":")
        dst = dst if sep else src
        if src.endswith("/*"):
            if dst != src:
                raise ValueError(f"Unsupported wildcard rename: {spec}")
            folder = stock / src[:-2]
            if not folder.is_dir() or folder.is_symlink():
                raise FileNotFoundError(folder)
            for path in folder.rglob("*"):
                if path.is_file() or path.is_symlink():
                    name = path.relative_to(stock).as_posix()
                    result[name] = name
        elif (stock / src).is_file() or (stock / src).is_symlink():
            result[dst] = src
        else:
            # The common list still contains files for other hi3660 products.
            # A missing C00 blob must never be substituted with a guessed one.
            print(f"not in C00: {src}")
    return result


def fixup(relative, path):
    name = str(relative)
    args = []
    if name in ("vendor/lib/hw/gralloc.hi3660.so", "vendor/lib64/hw/gralloc.hi3660.so"):
        args = ["--add-needed", "libhidlbase.so"]
    elif name == "vendor/lib64/hw/audio.primary_hisi.hi3660.so":
        subprocess.run([str(PATCHELF), "--add-needed", "libprocessgroup.so", str(path)], check=True)
        subprocess.run([str(PATCHELF), "--add-needed", "libshim_audioparams.so", str(path)], check=True)
        path.write_bytes(path.read_bytes().replace(b"str_parms_get_str", b"str_parms_get_mod"))
    elif name == "vendor/lib64/libbt-vendor.so":
        args = ["--set-soname", "libbt-vendor.so"]
    elif name in ("vendor/lib/hw/hwcomposer.hi3660.so", "vendor/lib64/hw/hwcomposer.hi3660.so"):
        args = ["--replace-needed", "libui.so", "libui-v28.so"]
    elif name == "vendor/lib64/libcamera_algo.so":
        args = ["--add-needed", "libui_shim.so"]
    elif name in ("vendor/lib64/libdcamera_effect.so", "vendor/lib64/libRefocusContrastPosition.so"):
        args = ["--add-needed", "liblogshim.so"]
    elif name == "vendor/lib64/hw/vendor.huawei.hardware.hwdisplay.displayengine@1.2-impl.so":
        args = ["--replace-needed", "displayeffect.kirin970.so", "displayeffect.hi3660.so"]
    elif name == "vendor/lib64/displayeffect.hi3660.so":
        args = ["--set-soname", "displayeffect.hi3660.so"]
    if args:
        subprocess.run([str(PATCHELF), *args, str(path)], check=True)
    if camera_config(name):
        path.write_bytes(compatible_camera_config(path.read_bytes()))
    if name in ("odm/lib64/hwcam/hwcam.hi3660.m.CMR.so",
                "odm/lib64/hwcam/hwcam.hi3660.m.SHT.so"):
        for needed in ("vendor.huawei.hardware.ai@1.0.so",
                       "vendor.huawei.hardware.biometrics.hwsecurefacerecognize@1.0.so"):
            subprocess.run([str(PATCHELF), "--remove-needed", needed, str(path)], check=True)


def fixup_supl_dex(dex, work):
    """Replace Android 9's package-private byte-array intrinsic with the public API."""
    path = work / "classes.dex"
    path.write_bytes(dex)
    listing = subprocess.run([str(DEXDUMP), "-d", str(path)], check=True,
                             capture_output=True, text=True).stdout
    public = "Ljava/lang/System;.arraycopy:(Ljava/lang/Object;ILjava/lang/Object;II)V // method@0500"
    private = "Ljava/lang/System;.arraycopy:([BI[BII)V // method@0501"
    if public not in listing:
        raise ValueError("C00 SUPL dex lacks the public System.arraycopy method")
    offsets = [int(match.group(1), 16) for match in re.finditer(
        r"(?m)^([0-9a-f]+):.*\|[0-9a-f]+: invoke-static .*"
        + re.escape(private) + r"$", listing)]
    if len(offsets) != 133 or listing.count("method@0501") != len(offsets):
        raise ValueError("Unexpected C00 SUPL System.arraycopy call sites")
    fixed = bytearray(dex)
    for offset in offsets:
        if fixed[offset] != 0x71 or struct.unpack_from("<H", fixed, offset + 2)[0] != 0x501:
            raise ValueError(f"Unexpected SUPL invoke at dex offset {offset:#x}")
        struct.pack_into("<H", fixed, offset + 2, 0x500)
    fixed[12:32] = hashlib.sha1(fixed[32:]).digest()
    struct.pack_into("<I", fixed, 8, zlib.adler32(fixed[12:]) & 0xffffffff)
    return bytes(fixed)


def rebuild_supl_apk(apk, vdex):
    """Restore code stripped from the preoptimized C00 system APK."""
    with tempfile.TemporaryDirectory(prefix="cmr-supl-") as work:
        work = Path(work)
        local_vdex = work / vdex.name
        shutil.copyfile(vdex, local_vdex)
        subprocess.run([str(VDEX_EXTRACTOR), "-i", str(local_vdex), "-o", str(work)],
                       check=True, stdout=subprocess.DEVNULL)
        cdex = work / (vdex.stem + "_classes.cdex")
        subprocess.run([str(COMPACT_DEX_CONVERTER), "-v", str(cdex)],
                       check=True, stdout=subprocess.DEVNULL)
        dex = (work / (cdex.name + ".new")).read_bytes()
        dex = fixup_supl_dex(dex, work)
    if not dex.startswith(b"dex\n") or b"Lcom/android/supl/SuplApplication;" not in dex:
        raise ValueError("C00 SUPL VDEX did not yield the expected application code")
    output = BytesIO()
    with zipfile.ZipFile(apk) as source, zipfile.ZipFile(output, "w") as target:
        for entry in source.infolist():
            # The stock signature covers the APK without a dex; Soong signs the rebuilt APK.
            if entry.filename.startswith("META-INF/") or entry.filename == "classes.dex":
                continue
            target.writestr(entry, source.read(entry.filename))
        entry = zipfile.ZipInfo("classes.dex", date_time=(2009, 1, 1, 0, 0, 0))
        entry.compress_type = zipfile.ZIP_STORED
        entry.external_attr = 0o644 << 16
        target.writestr(entry, dex)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="C00 capture's files/ directory")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--report", type=Path,
                        help="Write verified source and installed output SHA-256 values")
    args = parser.parse_args()
    stock = args.source.resolve(strict=True)
    if stock.name != "files" or not (stock.parent / "metadata/FILES_SHA256SUMS").is_file():
        parser.error("Expected the verified C00 capture files/ directory")
    expected = {}
    for line in (stock.parent / "metadata/FILES_SHA256SUMS").read_text().splitlines():
        sha, name = line.split(maxsplit=1)
        expected[name] = sha
    entries = selections(stock)
    sys.dont_write_bytecode = True
    from importlib.util import spec_from_file_location, module_from_spec
    spec = spec_from_file_location("c00_radio_extract", DEVICE / "extract-files.py")
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    changed = []
    for dst, src in sorted(entries.items()):
        # Reuse the radio extractor's safe Android path resolver.
        original = module.resolve_android(stock, src)
        resolved = original.relative_to(stock).as_posix()
        if digest(original) != expected.get(resolved):
            raise ValueError(f"Capture checksum mismatch: {resolved}")
        target = COMMON / dst
        if target.is_file() and camera_config(dst) and target.read_bytes() == compatible_camera_config(original.read_bytes()):
            continue
        if target.is_file() and digest(target) == digest(original) and not needs_fixup(dst):
            continue
        changed.append((dst, original))
    print(f"Verified {len(entries)} sources; {len(changed)} destinations need C00 data.")
    supl = module.resolve_android(stock, SUPL_SOURCE)
    supl_name = supl.relative_to(stock).as_posix()
    if digest(supl) != expected.get(supl_name):
        raise ValueError(f"Capture checksum mismatch: {supl_name}")
    supl_vdex = module.resolve_android(stock, SUPL_VDEX)
    supl_vdex_name = supl_vdex.relative_to(stock).as_posix()
    if digest(supl_vdex) != expected.get(supl_vdex_name):
        raise ValueError(f"Capture checksum mismatch: {supl_vdex_name}")
    rebuilt_supl = rebuild_supl_apk(supl, supl_vdex)
    supl_changed = not SUPL_TARGET.is_file() or SUPL_TARGET.read_bytes() != rebuilt_supl
    print(f"C00 GNSS SUPL APK update needed: {supl_changed}.")
    media = module.resolve_android(stock, MEDIA_SOURCE)
    media_name = media.relative_to(stock).as_posix()
    if digest(media) != expected.get(media_name):
        raise ValueError(f"Capture checksum mismatch: {media_name}")
    media_changed = not MEDIA_TARGET.is_file() or digest(MEDIA_TARGET) != digest(media)
    print(f"C00 camera media profiles update needed: {media_changed}.")
    if args.apply:
        for dst, original in changed:
            target = COMMON / dst
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + ".c00-tmp")
            shutil.copyfile(original, temporary)
            temporary.chmod(0o755 if "/bin/" in dst else 0o644)
            try:
                fixup(dst, temporary)
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
        for dst in RETIRED:
            (COMMON / dst).unlink(missing_ok=True)
        if supl_changed:
            SUPL_TARGET.parent.mkdir(parents=True, exist_ok=True)
            SUPL_TARGET.write_bytes(rebuilt_supl)
        if media_changed:
            shutil.copyfile(media, MEDIA_TARGET)
        for path in (COMMON / "odm/lib64/hwcam").glob("*.SHT.*.so"):
            path.unlink()
        (COMMON / "odm/lib64/hwcam/hwcam.hi3660.m.SHT.so").unlink(missing_ok=True)
        # Old regional configs and SHT sensor plugins do not belong to CMR-AL09.
        for folder in ("vendor/etc/gnss", "odm/etc/camera"):
            keep = {dst for dst in entries if dst.startswith(folder + "/")}
            for path in (COMMON / folder).rglob("*"):
                if path.is_file() and path.relative_to(COMMON).as_posix() not in keep:
                    path.unlink()
        print(f"Installed {len(changed)} C00 files and retired {len(RETIRED)} old service files.")
    if args.report:
        records = []
        for dst, src in sorted(entries.items()):
            original = module.resolve_android(stock, src)
            output = COMMON / dst
            records.append({"source": src,
                            "source_sha256": expected[original.relative_to(stock).as_posix()],
                            "destination": dst,
                            "output_sha256": digest(output) if output.is_file() else None})
        records.append({"source": SUPL_SOURCE, "source_sha256": expected[supl_name],
                        "vdex_source": SUPL_VDEX, "vdex_sha256": expected[supl_vdex_name],
                        "destination": SUPL_TARGET.relative_to(ANDROID).as_posix(),
                        "output_sha256": digest(SUPL_TARGET) if SUPL_TARGET.is_file() else None})
        records.append({"source": MEDIA_SOURCE, "source_sha256": expected[media_name],
                        "destination": MEDIA_TARGET.relative_to(ANDROID).as_posix(),
                        "output_sha256": digest(MEDIA_TARGET) if MEDIA_TARGET.is_file() else None})
        report = {"capture": stock.parent.name,
                  "source_manifest_sha256": digest(stock.parent / "metadata/FILES_SHA256SUMS"),
                  "files": records}
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(f"Wrote {len(records)} source/output hashes to {args.report}.")


def needs_fixup(name):
    return name in {
        "vendor/lib/hw/gralloc.hi3660.so", "vendor/lib64/hw/gralloc.hi3660.so",
        "vendor/lib/hw/hwcomposer.hi3660.so", "vendor/lib64/hw/hwcomposer.hi3660.so",
        "vendor/lib64/libcamera_algo.so", "vendor/lib64/libdcamera_effect.so",
        "vendor/lib64/libRefocusContrastPosition.so",
        "vendor/lib64/hw/vendor.huawei.hardware.hwdisplay.displayengine@1.2-impl.so",
        "vendor/lib64/displayeffect.hi3660.so", "odm/lib64/hwcam/hwcam.hi3660.m.CMR.so",
        "vendor/lib64/hw/audio.primary_hisi.hi3660.so", "vendor/lib64/libbt-vendor.so",
    }


if __name__ == "__main__":
    main()
