"""PlatformIO extra_script: injects FIRMWARE_VERSION from env var or version.txt."""
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

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", '\\"' + version + '\\"')])
