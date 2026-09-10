# Changelog

## [1.4.0] - 2026-09-10

### Changed

- Migrated the repository license from MIT to Apache License 2.0 and added
  `NOTICE` for project ownership, BSP/teaching-material attribution, and
  third-party boundaries.
- Established `VERSION` as the release-version SSOT and synchronized the
  reference firmware banner, display page, protocol examples, build output,
  and public documentation to `v1.4.0`.
- Added `tools/check-release.py` so CI verifies the current tree's license,
  notice, version references, and first-party SPDX headers.

### Added

- Published the IAP probe as part of the stable SDK release.

### Capability boundary

- `examples/iap-probe` demonstrates **single-sector self-write/erase/read
  capability** on real hardware, including rejection of an out-of-range IAP
  address. It does not implement a complete OTA update pipeline: transfer,
  verification beyond the probe sector, slot selection, rollback recovery,
  and boot selection remain unimplemented and unverified.
- The probe retains its `0xE000` code-size gate so its executable code cannot
  overlap the sector it erases.

### Verification

- Existing Python compile checks and CI presence checks pass.
- Keil builds and the IAP probe's code-size gate are verified locally without
  flashing either the probe or the resident firmware.
