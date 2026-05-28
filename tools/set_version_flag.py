"""PlatformIO extra_script: injects FIRMWARE_VERSION from version.txt."""
Import("env")  # noqa: F821 (PlatformIO provides this)
import os

try:
    ver_path = os.path.join(env.subst("$PROJECT_DIR"), "..", "..", "version.txt")
    with open(ver_path) as f:
        version = f.read().strip()
except Exception:
    version = "dev"

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", '\\"' + version + '\\"')])
