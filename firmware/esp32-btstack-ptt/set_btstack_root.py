#  PlatformIO pre-script: point BTSTACK_ROOT at the vendored BTstack tree.
#
#  The port's components/btstack/CMakeLists.txt resolves the BTstack source
#  root from $ENV{BTSTACK_ROOT} (its fallback assumes the native port layout,
#  which PlatformIO does not use). Set it here before CMake runs.
import os

Import("env")  # noqa: F821  (injected by PlatformIO)

os.environ["BTSTACK_ROOT"] = os.path.join(env["PROJECT_DIR"], "btstack")  # noqa: F821
