#pragma once
// Fallback for IDE/direct compilation only.
// During actual builds, set_version_flag.py generates a real firmware_version.h
// in $BUILD_DIR/generated/ which takes priority (prepended to CPPPATH).
#ifndef FIRMWARE_VERSION
#  define FIRMWARE_VERSION "dev"
#endif
