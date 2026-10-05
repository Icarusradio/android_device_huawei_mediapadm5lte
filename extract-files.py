#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Extract the CMR-AL09 C00 radio set from a verified offline capture.

This uses the existing vendor makefiles, including their separate libril module.
It does not invoke hi3660/extract-files.sh (which targets another device).
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil


DEVICE = Path(__file__).resolve().parent
ANDROID = DEVICE.parents[2]
VENDOR = Path("vendor/huawei/mediapadm5lte")
COMMON = Path("vendor/huawei/hi3660/proprietary")
RIL = {"vendor/lib/libril.so", "vendor/lib64/libril.so"}
# These foreign customization paths have a literal leading space in stock.
# Android PRODUCT_COPY_FILES cannot represent them; C00 selects all/cn instead.
EXCLUDED = {
    f"odm/hw_odm/{sku}/ncfg/eagsedu/ eg/COMMON.bin"
    for sku in ("CMR-AL09", "CMR-AL19")
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_relative(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ValueError(f"Unsafe relative path: {name}")
    return Path(path)


def resolve_android(root, relative):
    """Resolve Android absolute links inside the dump, never on the host."""
    pending = list(checked_relative(relative).parts)
    parts = []
    hops = 0
    while pending:
        part = pending.pop(0)
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise ValueError(f"Link escapes capture: {relative}")
            parts.pop()
            continue
        candidate = root.joinpath(*parts, part)
        if candidate.is_symlink():
            hops += 1
            if hops > 40:
                raise ValueError(f"Symlink loop: {relative}")
            target = PurePosixPath(os.readlink(candidate))
            if target.is_absolute():
                parts = []
                target = target.relative_to("/")
            pending = list(target.parts) + pending
        else:
            parts.append(part)
    result = root.joinpath(*parts)
    if not result.is_file():
        raise FileNotFoundError(f"Missing source: {relative} -> {result}")
    return result


def expand_list(source, filename):
    for line in filename.read_text().splitlines():
        spec = line.strip()
        if not spec or spec.startswith("#"):
            continue
        src, _, dst = spec.partition(":")
        dst = dst or src
        checked_relative(src)
        checked_relative(dst)
        if src.endswith("/*") and dst == src:
            directory = source / src[:-2]
            if not directory.is_dir() or directory.is_symlink():
                raise ValueError(f"Missing or linked source directory: {src}")
            count = 0
            for parent, dirs, files in os.walk(directory, followlinks=False):
                for name in dirs:
                    if (Path(parent) / name).is_symlink():
                        raise ValueError(f"Directory link requires explicit mapping: {parent}/{name}")
                for name in sorted(files):
                    relative = (Path(parent) / name).relative_to(source).as_posix()
                    yield relative, relative
                    count += 1
            if not count:
                raise ValueError(f"Empty source selection: {src}")
        elif any(c in spec for c in "*?[;|"):
            raise ValueError(f"Unsupported extraction spec: {spec}")
        else:
            yield src, dst


def read_checksums(path):
    result = {}
    for line in path.read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        name = name.removeprefix("*").removeprefix("./")
        checked_relative(name)
        if len(digest) != 64:
            raise ValueError(f"Invalid SHA-256 entry: {name}")
        result[name] = digest
    return result


def copy_file(source, destination, mode):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)


def install(prepared, backup, roots):
    """Keep each old tree/file and restore it if installation fails."""
    completed = []
    try:
        for relative in roots:
            target = ANDROID / relative
            old = backup / relative
            old.parent.mkdir(parents=True, exist_ok=True)
            existed = target.exists() or target.is_symlink()
            if existed:
                target.rename(old)
            completed.append((target, old, existed))
            new = prepared / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if new.is_dir():
                shutil.copytree(new, target)
            else:
                shutil.copy2(new, target)
    except BaseException:
        for target, old, existed in reversed(completed):
            if target.is_dir() and not target.is_symlink():
                shutil.rmtree(target)
            elif target.exists() or target.is_symlink():
                target.unlink()
            if existed:
                old.rename(target)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Capture files/ directory")
    parser.add_argument("--work-dir", required=True, type=Path,
                        help="New private work directory outside the source tree, on the same filesystem")
    parser.add_argument("--apply", action="store_true",
                        help="Install after verification; default only prepares and records files")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    work = args.work_dir.resolve()
    if work.is_relative_to(source) or work.is_relative_to(ANDROID):
        parser.error("Keep --work-dir outside both the capture and the Android tree")
    checksum_file = source.parent / "metadata/FILES_SHA256SUMS"
    expected = read_checksums(checksum_file)
    selections = []
    roots = [VENDOR / "proprietary"] + [VENDOR / name for name in sorted(RIL)]
    for src, dst in expand_list(source, DEVICE / "proprietary-files.txt"):
        relative = VENDOR / dst if dst in RIL else VENDOR / "proprietary" / dst
        selections.append((src, dst, relative))
    for src, dst in expand_list(source, DEVICE / "proprietary-radio-common.txt"):
        relative = COMMON / dst
        selections.append((src, dst, relative))
        roots.append(relative)
    if len({str(x[2]) for x in selections}) != len(selections):
        raise ValueError("Duplicate destination in extraction lists")

    work.mkdir(mode=0o700, parents=True, exist_ok=False)
    if work.stat().st_dev != (ANDROID / VENDOR).stat().st_dev:
        raise ValueError("Work directory must share the vendor filesystem for rollback")
    prepared = work / "prepared"
    records = []
    excluded = []
    for src, dst, relative in sorted(selections):
        original = resolve_android(source, src)
        resolved = original.relative_to(source).as_posix()
        digest = sha256(original)
        if expected.get(resolved) != digest:
            raise ValueError(f"Source checksum missing or mismatched: {resolved}")
        if src in EXCLUDED:
            excluded.append({"source": src, "source_sha256": digest,
                             "reason": "Non-C00 eagsedu customization with a space in its path"})
            continue
        if any(c.isspace() for c in str(relative)):
            raise ValueError(f"Android make cannot package a whitespace path: {relative}")
        output = prepared / relative
        mode = 0o755 if "/bin/" in dst else 0o644
        copy_file(original, output, mode)
        output_digest = sha256(output)
        if output_digest != digest:
            raise ValueError(f"Copy verification failed: {relative}")
        old = ANDROID / relative
        records.append({"source": src, "resolved_source": resolved,
                        "destination": relative.as_posix(), "source_sha256": digest,
                        "output_sha256": output_digest,
                        "previous_sha256": sha256(old) if old.is_file() else None,
                        "mode": oct(mode), "binary_fixups": []})

    destinations = {r["destination"] for r in records}
    removed = []
    for old in (ANDROID / VENDOR / "proprietary").rglob("*"):
        if old.is_file() or old.is_symlink():
            rel = old.relative_to(ANDROID).as_posix()
            if rel not in destinations:
                removed.append({"path": rel, "sha256": sha256(old) if old.is_file() else None})
    report = {"source": str(source), "checksum_manifest_sha256": sha256(checksum_file),
              "files": records, "removed_from_device_proprietary": sorted(removed, key=lambda x: x["path"]),
              "excluded": excluded,
              "installed": False, "backup": str(work / "before-extraction"),
              "lists_sha256": {p.name: sha256(p) for p in
                               [DEVICE / "proprietary-files.txt", DEVICE / "proprietary-radio-common.txt"]}}
    report_path = work / "extraction.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    if args.apply:
        install(prepared, work / "before-extraction", roots)
        report["installed"] = True
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{'Installed' if args.apply else 'Prepared'} {len(records)} verified files; "
          f"{len(removed)} obsolete paths removed{' on apply' if not args.apply else ''}.")
    print(f"Manifest: {report_path}")


if __name__ == "__main__":
    main()
