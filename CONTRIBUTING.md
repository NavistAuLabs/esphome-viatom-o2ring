# Contributing

Outside contributions are welcome. This document covers how this repo relates
to where the component is developed, how to build and test a change, the
comment and documentation discipline this codebase runs on, and the one rule
that matters more than any of them: no real recordings in issues or pull
requests.

## Never paste real data

The files this component pulls are overnight pulse-oximetry recordings — your
oxygen saturation and heart rate, minute by minute, while you sleep. Do not
attach `.vld` files, screenshots of their contents, or logs containing them to
an issue or pull request. Redact ring serial numbers and BLE MAC addresses
too: the serial is the directory name recordings are filed under, and the MAC
identifies a specific device.

If a bug can only be shown with real data, say so in the issue and it will be
worked out privately. Describing the shape of the problem — byte counts,
offsets, which field is wrong — is almost always enough.

## Where this is developed

This repository is a curated export. The component is maintained in a private
repo alongside protocol research, packet captures and real recordings that
are not published and will not be. A merged pull request is folded back into
that repo, and a later release may arrive here as a single export commit
rather than as your commit replayed.

Practically, that means: keep changes inside `components/viatom_o2ring/` and
the repo's own documentation, and expect review to care about whether a claim
is *measured* (see below).

## Branch model and releases

- Feature branches (`feat/…`, `fix/…`, `docs/…`) target `main`.
- Releases are git tags only. There are no build artifacts — ESPHome consumes
  the component straight from a tag via
  `source: github://NavistAuLabs/esphome-viatom-o2ring@vX.Y.Z`.
- Because a tag is what users pin, a tag is never moved once pushed.

## Build and test

There is no test suite, and pretending otherwise would be worse than saying
so. Verification is compiling the component into a real configuration and
running it against a real ring:

```sh
esphome config   your-device.yaml     # schema changes: validates the Python side
esphome compile  your-device.yaml     # C++ changes: this is the real check
esphome run      your-device.yaml
```

Then watch it work. `logger: level: VERBOSE` is the useful level — the
component logs every advertisement it sees at `VERBOSE`, which is how you
tell "the listener is not being called" apart from "the ring is asleep".
Note that ESPHome compiles `ESP_LOGVV` statements out entirely at `VERBOSE`,
so never add one expecting to see it.

`ViatomPacketCodec` is deliberately a plain byte-in/byte-out class with no
BLE or ESPHome dependency, precisely so the framing half can be reasoned
about and eventually tested on its own. A pull request that adds real tests
for it is very welcome; one that gives it a dependency on the connection
state machine is not.

If you cannot test against a ring, say so in the pull request. An untested
change is still reviewable — an untested change *described* as verified is
not.

## Style

Match the surrounding code. It follows ESPHome's own conventions:

- C++: ESPHome's `clang-format` (Google style, 2-space indent, 120 columns).
- Python: ESPHome's component conventions — `CONF_*` constants, schema first,
  `to_code` last.
- YAML in documentation: 2-space indent.

## Measured versus inferred

This codebase's comments and documentation carefully separate what was
observed from what was guessed. The `.vld` field table marks which bytes are
verified and which labels are inherited from an earlier published document
and never confirmed. The device-behaviour section says how each fact was
observed, and where something is unknown it says so rather than filling the
gap with a plausible-sounding reason.

That split is the expensive part of this project, and it is easy to destroy
by accident. So:

- Don't promote an inferred label to a verified one without saying what you
  did to verify it.
- Don't delete a comment explaining *why* something is the way it is because
  it looks like noise. Several of them are load-bearing (release the BLE link
  on every terminal path; the presence timeout must exceed the connect
  timeout; `FileList` is authoritative and `FILE_OPEN` lies).
- If you find one of those claims is wrong, correcting it is a genuinely
  valuable contribution — bring the evidence.

## Documentation

`components/viatom_o2ring/README.md` is the protocol and device reference.
The root `README.md` is for someone putting the component on a board. A change
that alters observable behaviour updates the one that describes it.

Examples in documentation use synthetic values — `AA:BB:CC:DD:EE:FF` for a
MAC, made-up figures for anything derived from a recording. Keep it that way.

## CHANGELOG discipline

This project follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Add your entry to `CHANGELOG.md` under `## [Unreleased]` in the same pull
request that makes the change, not at release time. Any change to what gets
written to the SD card — paths, the staging and promotion rule, when a file
is considered complete — is called out explicitly regardless of size, since
those are the changes that can silently corrupt a recording that cannot be
fetched again.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). Reports
go to `foss+conduct@navist.com.au`.
