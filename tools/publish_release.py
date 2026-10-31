"""Publish a firmware release on this repository's `releases` branch.

    python tools/publish_release.py [firmware.bin] [--push] [--trailer=TEXT]

Copies the built image (default .pio/build/harkenn-speaker/firmware.bin) to v<version>/firmware.bin on the
`releases` branch, points latest.json at it and commits; --push also pushes the branch. The version and board are
read from the image's HARKENN-FW marker (src/main.cpp), so bump FW_VERSION in platformio.ini first. A Harkenn
server reads latest.json from this branch and offers newer firmware to speakers when they start.

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
WORKTREE = ROOT / ".releases"
BRANCH = "releases"
MARKER = re.compile(rb"HARKENN-FW\|([0-9A-Za-z.+_-]{1,32})\|([0-9A-Za-z._-]{1,64})\|")


def git(*args: str, cwd: Path = ROOT, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=cwd, check=check, capture_output=True, text=True)


def version_key(v: str) -> tuple[int, ...]:
    return tuple(int(p) for p in re.findall(r"\d+", re.split(r"[-+]", v, maxsplit=1)[0])[:3])


def inspect(data: bytes) -> tuple[str, str]:
    if len(data) < 1024 or data[0] != 0xE9:
        sys.exit("not an ESP32 app image (use firmware.bin, not the merged image)")
    m = MARKER.search(data)
    if not m:
        sys.exit("no HARKENN-FW marker: not Harkenn speaker firmware")
    return m.group(1).decode(), m.group(2).decode()


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
    image = Path(args[0]) if args else DEFAULT_IMAGE
    data = image.read_bytes()
    version, hardware = inspect(data)
    wt = worktree()
    manifest_path = wt / "latest.json"
    if manifest_path.exists():
        current = json.loads(manifest_path.read_text())["version"]
        if version_key(version) <= version_key(current):
            sys.exit(f"{image} is {version}, not newer than the published {current}: bump FW_VERSION first")
    target = wt / f"v{version}" / "firmware.bin"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    manifest = {"version": version, "hardware": hardware, "file": f"v{version}/firmware.bin", "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    git("add", "latest.json", str(target.relative_to(wt)), cwd=wt)
    trailers = [f"--trailer={a[10:]}" for a in sys.argv[1:] if a.startswith("--trailer=")]
    git("commit", "-q", "-m", f"Firmware {version}", *trailers, cwd=wt)
    print(f"committed firmware {version} ({len(data)} bytes, {hardware}) on {BRANCH}")
    if "--push" in sys.argv:
        git("push", "-q", "origin", BRANCH, cwd=wt)
        print("pushed")


if __name__ == "__main__":
    main()
