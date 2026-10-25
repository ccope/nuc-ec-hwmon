#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile/run the real reader with fake kernel APIs; no module or hardware I/O."""
import os
from pathlib import Path
import resource
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="nuc-ec-hwmon-tests-") as directory:
    binary = Path(directory) / "acpi-reader-test"
    compiler = shlex.split(os.environ.get("HOSTCC", "cc"))
    subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                              "-I" + str(root / "tests/include"), "-I" + str(root),
                              str(root / "tests/acpi_reader_test.c"), "-o", str(binary)],
                   check=True)
    # Assertion failures must not leave a core dump on the build machine.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    subprocess.run([str(binary)], check=True)
