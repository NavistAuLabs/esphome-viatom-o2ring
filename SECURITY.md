# Security Policy

`viatom_o2ring` parses packets from an unauthenticated BLE peer and writes the
result to an SD card on your ESP32. Treat as a security issue any bug that
lets crafted BLE traffic write outside the paths this component documents,
corrupt memory on the board, or expose a recording somewhere it shouldn't go —
recordings are personal health data.

## Supported versions

Only the latest release receives security fixes.

| Version | Supported |
| ------- | --------- |
| latest 0.x.y | :white_check_mark: |
| older 0.x.y  | :x: |

## Reporting a vulnerability

Please report security vulnerabilities privately via
[GitHub Security Advisories](https://github.com/NavistAuLabs/esphome-viatom-o2ring/security/advisories/new)
for this repository — do not open a public issue.

Do not attach real recordings or logs containing them. Describe the packet
shape that triggers the bug; synthetic bytes are enough to reproduce anything
in the framing layer.

We aim to acknowledge reports within **7 days**. Once a fix is available,
we'll coordinate disclosure timing with you.

## In scope

- Packet reassembly and framing (`ViatomPacketCodec`): anything where a
  crafted or malformed notification stream causes an out-of-bounds read or
  write, or an unbounded allocation.
- The download path: anything that lets a device-supplied filename or size
  escape `/sdcard/o2ring/<serial>/` and `/sdcard/.partial/o2ring/<serial>/`,
  or that promotes an incomplete transfer into the completed tree.
- Anything that makes the component hold the BLE link open on a terminal
  path. That is a safety problem as well as a correctness one: a held link
  stops the ring advertising and can preempt its own save sequence.

## Out of scope

- The ring's own protocol having no authentication or encryption. That is how
  the hardware ships; it is the premise of this component, not a defect in it.
- ESPHome, ESP-IDF, or the `sd_mmc_card` component — report those upstream to
  their maintainers. A heads-up here is still welcome so this project can
  track and update, but the fix belongs upstream.
- Physical access to the SD card. Anyone holding the card holds the
  recordings; this component does not encrypt them.
