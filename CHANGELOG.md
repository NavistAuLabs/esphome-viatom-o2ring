# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Any change to what gets written to the SD card — the paths, the staging and
promotion rule, or when a transfer counts as complete — is called out here
regardless of size, since those are the changes that can silently corrupt a
recording the ring has already rotated away.

## [Unreleased]

## [0.2.0] - 2026-11-15

### Breaking

- `min_sync_interval` is removed, and so is the trigger it limited (a sync on
  every reappearance of the ring after 60 s off air). A configuration that
  still sets it fails validation: delete the line. By default the component
  now never connects on its own; see `start_sync()` and `auto_sync` below.

### Added

- `start_sync()`: starts a sync on demand, for example from a template button
  or an automation. Ignored while a sync is already running or the BLE
  client is busy.
- `auto_sync` (default `false`) and `off_air_threshold` (default `2h`):
  opt-in automatic sync that connects once when the ring appears after being
  off air continuously for `off_air_threshold`, retries once on failure, and
  then stays silent until the ring has been off air that long again. Every
  connect costs the ring's battery, so off is the recommended setting.

### Fixed

- A link that drops mid-walk now ends the sync, instead of leaving the
  component believing a sync is still running and refusing every later one.
- The build no longer fails with `sensor/sensor.h: No such file or
  directory` on a board that configures no other sensor: the component now
  auto-loads `sensor` and `text_sensor`.

## [0.1.0] - 2026-09-18

First public release.

### Added

- `viatom_o2ring`: an ESPHome external component that pulls stored recordings
  off a Wellue/Viatom O2Ring over BLE and writes them to an SD card,
  byte-identical to what the ring serves.
- Sync is triggered by the ring's advertisements going from absent to
  present — which, after a night's wear, means it was just docked — rather
  than by a timer. `min_sync_interval` (default `30min`) is a floor between
  attempts, not a schedule.
- Walks the ring's whole `FileList` oldest-first, because the ring's four
  storage slots evict the oldest recording when a new session *starts*.
  Files already on the card are skipped, so an interrupted walk resumes.
- Downloads are staged under `/sdcard/.partial/o2ring/<serial>/` and promoted
  to `/sdcard/o2ring/<serial>/` only once the bytes written match the size
  the ring declared at `FILE_OPEN`.
- Optional entities: `battery_level`, `serial_number`, `state`, `file_list`,
  and `last_sync` — the ring's own clock from the last walk that finished
  with no failed transfer, which is what distinguishes "nothing new to fetch"
  from "failed to fetch it".
- The BLE link is released on every terminal path, not just on success: a
  connected peripheral stops advertising, so a held link makes the ring
  invisible and can preempt its own save sequence.
- Connect attempts are retried up to 4 times, 3 seconds apart, to absorb the
  frequent locally-cancelled connect failures the ESP32 BLE stack produces.
- `components/viatom_o2ring/README.md` documents the corrected reply framing
  (replies are marked `0x55` and byte 1 is a status, not a command echo), the
  corrected `.vld` layout, and the measured device behaviour — with verified
  fields separated from unverified inherited labels.

[Unreleased]: https://github.com/NavistAuLabs/esphome-viatom-o2ring/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/NavistAuLabs/esphome-viatom-o2ring/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/NavistAuLabs/esphome-viatom-o2ring/releases/tag/v0.1.0
