"""Publish a firmware release on this repository's `releases` branch.

    python tools/publish_release.py [firmware.bin] [--channel=stable|beta|alpha|development] [--push] [--trailer=TEXT]

Copies the built image (default .pio/build/harkenn-speaker/firmware.bin) to v<version>/firmware.bin on the
`releases` branch, points the channel's manifest at it and commits; --push also pushes the branch.

It also publishes the complete flash image PlatformIO builds next to it (firmware.factory.bin: bootloader,
partition table and app, written at 0x0) as v<version>/harkenn-speaker-<version>-install.bin, for a first install
over USB (see the README). A stable release also becomes install/harkenn-speaker.bin, a link that always gives the
newest stable install image. The version and
board are read from the image's HARKENN-FW marker (src/main.cpp), so bump FW_VERSION in platformio.ini first.

Channels and their manifests: stable latest.json, beta latest-beta.json, alpha latest-alpha.json, development
latest-development.json. Give pre-releases a semantic-version suffix that matches, e.g. 0.12.0-beta.1,
0.12.0-alpha.3, 0.12.0-development.7; stable versions have none. A Harkenn server follows one channel and offers the
newest firmware on it or on any more stable channel (app/updates.py in server-docker).

The branch is checked out in a git worktree at .releases/ (ignored), so the source checkout is never touched.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = ROOT / ".pio/build/harkenn-speaker/firmware.bin"
APP_OFFSET = 0x10000  # where the app sits in the install image (partitions.csv)
WORKTREE = ROOT / ".releases"
BRANCH = "releases"
CHANNELS = ("stable", "beta", "alpha", "development")
MARKER = re.compile(rb"HARKENN-FW\|([0-9A-Za-z.+_-]{1,32})\|([0-9A-Za-z._-]{1,64})\|")


def git(*args: str, cwd: Path = ROOT, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=cwd, check=check, capture_output=True, text=True)


def version_key(version: str) -> tuple:
    """Semantic-version order, as the server compares: 0.6.0-beta.2 < 0.6.0-beta.10 < 0.6.0."""
    v = version.strip().lstrip("vV").split("+", 1)[0]
    core, _, pre = v.partition("-")
    nums = [int(p) for p in re.findall(r"\d+", core)[:3]]
    nums += [0] * (3 - len(nums))
    if not pre:
        return (*nums, 1, ())
    return (*nums, 0, tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")))


def manifest_name(channel: str) -> str:
    return "latest.json" if channel == "stable" else f"latest-{channel}.json"


def inspect(data: bytes) -> tuple[str, str]:
    if len(data) < 1024 or data[0] != 0xE9:
        sys.exit("not an ESP32 app image (use firmware.bin, not the merged image)")
    m = MARKER.search(data)
    if not m:
        sys.exit("no HARKENN-FW marker: not Harkenn speaker firmware")
    return m.group(1).decode(), m.group(2).decode()


def install_image(app_path: Path, app: bytes) -> bytes | None:
    """The complete flash image built with this app, if it's there and really contains this app."""
    factory = app_path.with_name("firmware.factory.bin")
    if not factory.is_file():
        return None
    data = factory.read_bytes()
    if data[:1] != b"\xe9" or data[APP_OFFSET:APP_OFFSET + len(app)] != app:
        sys.exit(f"{factory} doesn't contain {app_path}: rebuild before publishing")
    return data


def worktree() -> Path:
    if (WORKTREE / ".git").exists():
        git("pull", "-q", "--ff-only", "origin", BRANCH, cwd=WORKTREE, check=False)
        return WORKTREE
    git("fetch", "-q", "origin", check=False)
    if git("rev-parse", "--verify", "-q", f"origin/{BRANCH}", check=False).returncode == 0:
        git("worktree", "add", "-q", "-B", BRANCH, str(WORKTREE), f"origin/{BRANCH}")
    else:  # first release: an orphan branch with no source files
        git("worktree", "add", "-q", "--detach", str(WORKTREE))
        git("checkout", "-q", "--orphan", BRANCH, cwd=WORKTREE)
        git("rm", "-rfq", ".", cwd=WORKTREE)
        (WORKTREE / "README.md").write_text(
            "# Harkenn speaker firmware releases\n\n`latest.json` names the current release; a Harkenn server reads it"
            " and offers newer firmware to speakers when they start.\n")
        git("add", "README.md", cwd=WORKTREE)
    return WORKTREE


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    channel = next((a[10:] for a in sys.argv[1:] if a.startswith("--channel=")), "stable")
    if channel not in CHANNELS:
        sys.exit(f"--channel must be one of {', '.join(CHANNELS)}")
    image = Path(args[0]) if args else DEFAULT_IMAGE
    data = image.read_bytes()
    version, hardware = inspect(data)
    pre = version.partition("-")[2]
    if channel == "stable" and pre:
        sys.exit(f"{version} is a pre-release: publish it on --channel=beta, alpha or development")
    if channel != "stable" and not pre.startswith(channel):
        sys.exit(f"for the {channel} channel, give FW_VERSION a -{channel}.N suffix (e.g. 0.12.0-{channel}.1)")
    wt = worktree()
    manifest_path = wt / manifest_name(channel)
    if manifest_path.exists():
        current = json.loads(manifest_path.read_text())["version"]
        if version_key(version) <= version_key(current):
            sys.exit(f"{image} is {version}, not newer than {current} on {channel}: bump FW_VERSION first")
    target = wt / f"v{version}" / "firmware.bin"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    manifest = {"version": version, "hardware": hardware, "file": f"v{version}/firmware.bin", "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}
    paths = [manifest_path.name, str(target.relative_to(wt))]
    full = install_image(image, data)
    if full is None:
        print("note: no firmware.factory.bin next to the image, so no install image this time")
    else:
        install = target.parent / f"harkenn-speaker-{version}-install.bin"
        install.write_bytes(full)
        manifest["install"] = {"file": str(install.relative_to(wt)).replace("\\", "/"), "size": len(full),
                               "sha256": hashlib.sha256(full).hexdigest()}
        paths.append(str(install.relative_to(wt)))
        if channel == "stable":
            latest = wt / "install" / "harkenn-speaker.bin"
            latest.parent.mkdir(exist_ok=True)
            latest.write_bytes(full)
            paths.append(str(latest.relative_to(wt)))
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    git("add", *paths, cwd=wt)
    trailers = [f"--trailer={a[10:]}" for a in sys.argv[1:] if a.startswith("--trailer=")]
    git("commit", "-q", "-m", f"Firmware {version} ({channel})", *trailers, cwd=wt)
    print(f"committed firmware {version} ({len(data)} bytes, {hardware}) on {BRANCH}, channel {channel}")
    if "--push" in sys.argv:
        git("push", "-q", "origin", BRANCH, cwd=wt)
        print("pushed")


if __name__ == "__main__":
    main()
