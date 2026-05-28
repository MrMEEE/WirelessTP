#!/usr/bin/env python3
"""
WirelessTP Release Manager

Automates the full release process:
  - Version bump (patch by default, or --minor / --major / --version X.Y.Z)
  - Updates version.txt
  - git commit → tag → push  (triggers GitHub Actions firmware build)

Usage:
    python tools/release.py                    # patch bump  (0.1.0 → 0.1.1)
    python tools/release.py --minor            # minor bump  (0.1.1 → 0.2.0)
    python tools/release.py --major            # major bump  (0.2.0 → 1.0.0)
    python tools/release.py --version 2.0.0    # explicit version
    python tools/release.py --dry-run          # preview — no changes written
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

# ── Project layout ─────────────────────────────────────────────────────────────
PROJECT_ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = PROJECT_ROOT / "version.txt"

# ──────────────────────────────────────────────────────────────────────────────


class ReleaseError(RuntimeError):
    pass


class ReleaseManager:
    def __init__(self, dry_run: bool = False) -> None:
        self.dry_run = dry_run
        self._changes: list[str] = []

    # ── Logging ───────────────────────────────────────────────────────────────

    def _log(self, msg: str, level: str = "INFO") -> None:
        prefix = "[DRY-RUN] " if self.dry_run else ""
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"{ts}  {prefix}{level}: {msg}")

    def info(self, msg: str) -> None: self._log(msg, "INFO")
    def ok(self,   msg: str) -> None: self._log(msg, "OK  ")
    def warn(self, msg: str) -> None: self._log(msg, "WARN")
    def error(self, msg: str) -> None: self._log(msg, "ERROR")

    # ── Shell helpers ─────────────────────────────────────────────────────────

    def _run(
        self,
        cmd: list[str],
        *,
        capture: bool = True,
        check: bool = True,
        read_only: bool = False,
    ) -> subprocess.CompletedProcess:
        self.info(f"$ {' '.join(cmd)}")
        if self.dry_run and not read_only:
            self._log("(skipped in dry-run mode)", "DEBUG")
            return subprocess.CompletedProcess(cmd, 0, "", "")
        try:
            result = subprocess.run(
                cmd,
                cwd=PROJECT_ROOT,
                capture_output=capture,
                text=True,
                check=check,
            )
            if capture and result.stdout.strip():
                self._log(result.stdout.strip(), "OUT ")
            return result
        except subprocess.CalledProcessError as exc:
            self.error(f"Command failed (exit {exc.returncode})")
            if exc.stderr:
                self.error(exc.stderr.strip())
            raise

    # ── Version parsing ───────────────────────────────────────────────────────

    @staticmethod
    def parse_version(s: str) -> tuple[int, int, int]:
        m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", s.strip())
        if not m:
            raise ReleaseError(f"Invalid version format: {s!r}  (expected X.Y.Z)")
        return int(m.group(1)), int(m.group(2)), int(m.group(3))

    @staticmethod
    def fmt(t: tuple[int, int, int]) -> str:
        return f"{t[0]}.{t[1]}.{t[2]}"

    def current_version(self) -> str:
        if not VERSION_FILE.exists():
            raise ReleaseError(f"{VERSION_FILE} not found — run from project root")
        return VERSION_FILE.read_text().strip()

    def bump(self, current: str, mode: str) -> str:
        maj, min_, pat = self.parse_version(current)
        if mode == "major":
            return self.fmt((maj + 1, 0, 0))
        if mode == "minor":
            return self.fmt((maj, min_ + 1, 0))
        return self.fmt((maj, min_, pat + 1))

    # ── Pre-flight checks ─────────────────────────────────────────────────────

    def check_git_state(self) -> None:
        branch = self._run(
            ["git", "branch", "--show-current"], read_only=True
        ).stdout.strip()
        if branch not in ("main", "master"):
            raise ReleaseError(
                f"Releases must be made from 'main' or 'master' (currently: {branch!r})"
            )

        # version.txt is managed by this script — allowed to be pre-modified
        managed = {str(VERSION_FILE.relative_to(PROJECT_ROOT))}

        raw_status = self._run(
            ["git", "status", "--porcelain"], read_only=True
        ).stdout
        lines = [l for l in raw_status.splitlines() if l.strip()]

        unmanaged = [l for l in lines if l[3:] not in managed]
        if unmanaged:
            raise ReleaseError(
                "Working tree has uncommitted changes:\n" + "\n".join(unmanaged)
            )

        ahead = self._run(
            ["git", "rev-list", "--count", f"origin/{branch}..HEAD"],
            read_only=True,
            check=False,
        ).stdout.strip()
        if ahead and ahead != "0":
            self.warn(
                f"{ahead} commit(s) ahead of origin/{branch} — "
                "they will be pushed with the tag."
            )

    def check_tag_doesnt_exist(self, version: str) -> None:
        tag = f"v{version}"
        existing = self._run(
            ["git", "tag", "-l", tag], read_only=True
        ).stdout.strip()
        if existing:
            raise ReleaseError(f"Tag {tag!r} already exists.")

    # ── File updates ──────────────────────────────────────────────────────────

    def update_version_file(self, new_version: str) -> None:
        self.info(f"Updating {VERSION_FILE.relative_to(PROJECT_ROOT)}")
        if not self.dry_run:
            VERSION_FILE.write_text(new_version + "\n")
        self._changes.append(str(VERSION_FILE.relative_to(PROJECT_ROOT)))

    # ── Git operations ────────────────────────────────────────────────────────

    def git_commit_tag_push(self, new_version: str) -> None:
        tag = f"v{new_version}"
        self._run(["git", "add"] + self._changes)
        self._run(["git", "commit", "-m", f"chore: release {new_version}"])
        self._run(["git", "tag", "-a", tag, "-m", f"Release {new_version}"])
        self._run(["git", "push", "origin", "HEAD"])
        self._run(["git", "push", "origin", tag])
        self.ok(f"Tag {tag} pushed — GitHub Actions will build the firmware package.")

    # ── Entrypoint ────────────────────────────────────────────────────────────

    def run(self, mode: str, explicit_version: str | None) -> None:
        if self.dry_run:
            self.warn("DRY-RUN mode — no files or git objects will be written.")

        current = self.current_version()
        if explicit_version:
            self.parse_version(explicit_version)
            new_version = explicit_version
        else:
            new_version = self.bump(current, mode)

        self.info(f"Current version : {current}")
        self.info(f"New version     : {new_version}")

        self.check_git_state()
        self.check_tag_doesnt_exist(new_version)
        self.update_version_file(new_version)
        self.git_commit_tag_push(new_version)

        self.ok(f"Release {new_version} complete.")


# ──────────────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="WirelessTP release manager",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    bump_group = parser.add_mutually_exclusive_group()
    bump_group.add_argument("--major",   action="store_true", help="Major version bump")
    bump_group.add_argument("--minor",   action="store_true", help="Minor version bump")
    bump_group.add_argument("--patch",   action="store_true", help="Patch version bump (default)")
    bump_group.add_argument("--version", metavar="X.Y.Z",     help="Explicit version")
    parser.add_argument("--dry-run", action="store_true", help="Preview only — no changes")
    args = parser.parse_args()

    if args.major:
        mode = "major"
    elif args.minor:
        mode = "minor"
    else:
        mode = "patch"

    mgr = ReleaseManager(dry_run=args.dry_run)
    try:
        mgr.run(mode, args.version)
    except ReleaseError as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nAborted.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
