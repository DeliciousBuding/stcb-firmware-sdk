#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate release metadata, license/notice state, and version references."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require_text(rel: str, needle: str) -> None:
    path = ROOT / rel
    if not path.is_file():
        raise SystemExit(f"[FAIL] missing {rel}")
    if needle not in path.read_text(encoding="utf-8"):
        raise SystemExit(f"[FAIL] {rel} does not contain {needle!r}")


def main() -> int:
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit(f"[FAIL] invalid VERSION: {version!r}")

    require_text("LICENSE", "Apache License")
    require_text("LICENSE", "Version 2.0, January 2004")
    require_text("NOTICE", "Copyright 2026 DeliciousBuding and contributors")
    require_text("NOTICE", "BSP and teaching-material attribution")
    require_text("NOTICE", "Third-party boundaries")
    require_text("NOTICE", "Apache License, Version 2.0")
    require_text("README.md", f"v{version}")
    require_text("README.en.md", f"v{version}")
    require_text("README.md", "Apache License 2.0")
    require_text("README.en.md", "Apache License 2.0")
    require_text("docs/stcb-device-protocol-v1.md", f"HELLO:stcb-full:v{version}:proto=1")
    require_text("examples/stcb-full/main.c", f"HELLO:stcb-full:v{version}:proto=1")
    require_text("examples/stcb-full/main.c", f"StCb{version.replace('.', '')}-")
    require_text("examples/stcb-full/README.md", f"# STC-B Full Firmware v{version}")
    require_text("examples/stcb-full/build.py", 'ROOT / "VERSION"')
    require_text("examples/stcb-full/build.py", "stcb-full v{VERSION}")
    require_text("CHANGELOG.md", f"## [{version}] - 2026-09-10")

    source_files = sorted(
        list((ROOT / "tools").glob("*.py"))
        + list((ROOT / "examples").glob("**/*.py"))
        + list((ROOT / "examples").glob("**/*.c"))
    )
    missing_spdx = []
    for path in source_files:
        if "SPDX-License-Identifier: Apache-2.0" not in path.read_text(encoding="utf-8"):
            missing_spdx.append(path.relative_to(ROOT).as_posix())
    if missing_spdx:
        raise SystemExit("[FAIL] missing SPDX Apache-2.0 header: " + ", ".join(missing_spdx))

    print(f"[OK] release v{version}, Apache-2.0 notice, version references, and SPDX headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
