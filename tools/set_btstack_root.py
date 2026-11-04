#  PlatformIO pre-script for the examples: point BTSTACK_ROOT at the BTstack
#  checkout in the repo root (tools/setup_btstack.sh).
#
#  The port's components/btstack/CMakeLists.txt resolves the BTstack source
#  root from $ENV{BTSTACK_ROOT} (its fallback assumes the native port layout,
#  which PlatformIO does not use). Set it here before CMake runs.
import os

Import("env")  # noqa: F821  (injected by PlatformIO)

os.environ["BTSTACK_ROOT"] = os.path.abspath(
    os.path.join(env["PROJECT_DIR"], "..", "..", "btstack"))  # noqa: F821
