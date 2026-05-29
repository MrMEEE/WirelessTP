"""PlatformIO extra_script: injects FIRMWARE_VERSION from env var or version.txt.

Generates a firmware_version.h header in the project's src/ directory so that
SCons tracks the version as a real file dependency. This guarantees recompilation
whenever the version string changes, even when source files are unchanged
(CPPDEFINES alone does not reliably trigger recompilation in that case).

The generated file is gitignored; shared/include/firmware_version.h provides
a fallback stub for IDE/direct compilation.
"""
Import("env")  # noqa: F821 (PlatformIO provides this)
import os

# Prefer an env var so CI can inject the tag version without relying on file paths.
version = os.environ.get("FIRMWARE_VERSION", "").strip()
if not version:
    try:
        ver_path = os.path.join(env.subst("$PROJECT_DIR"), "..", "..", "version.txt")
        with open(ver_path) as f:
            version = f.read().strip()
    except Exception:
        version = "dev"

# Write into src/ — PlatformIO adds -Isrc (before shared/include) so this file
# takes precedence over the IDE fallback stub in shared/include/.
# SCons tracks files in src/ as real dependencies and will recompile when this
# file changes, solving the stale-cache problem.
src_dir = env.subst("$PROJECT_SRC_DIR")
header_path = os.path.join(src_dir, "firmware_version.h")
content = '#pragma once\n#define FIRMWARE_VERSION "{}"\n'.format(version)
try:
    with open(header_path) as f:
        existing = f.read()
except Exception:
    existing = ""
if existing != content:
    with open(header_path, "w") as f:
        f.write(content)
