---
name: Bug report
about: Report a problem with the viatom_o2ring component
title: ""
labels: bug
assignees: ""
---

**IMPORTANT: never attach a real recording or a log containing one.**
`.vld` files are overnight pulse-oximetry data. Redact your ring's serial
number and BLE MAC address from anything you paste, and describe the shape of
the problem — byte counts, offsets, which field is wrong — rather than
pasting contents.

## Environment

- ESPHome version:
- Board and framework (e.g. ESP32-POE-ISO, esp-idf):
- SD component and version (e.g. `sd_mmc_card`):
- `viatom_o2ring` version or tag:
- Ring model:

## What happened

## What you expected to happen

## Steps to reproduce

Include what the ring was doing — worn, just removed, docked and charging,
docked while already full. Its state decides whether it is reachable at all.

## Relevant log output

Set `logger: level: VERBOSE` and paste the `viatom_o2ring` lines.
Redact the MAC address and serial number first.

```
```

## Configuration

Your `viatom_o2ring`, `ble_client` and SD blocks, with the MAC redacted.

```yaml
```
