<!--
Feature branches target `main`. See CONTRIBUTING.md.
Never include a real recording, ring serial, or BLE MAC in the diff or in
this description — examples use AA:BB:CC:DD:EE:FF and synthetic figures.
-->

## What this changes

<!-- One or two sentences. What behaviour is different after this merge? -->

## Why

<!-- The problem this solves. Link the issue if there is one. -->

Closes #

## How this was verified

<!--
Say what you actually ran. "Compiles" and "tested against a real ring" are
different claims — an untested change is reviewable, an untested change
described as verified is not.
-->

- [ ] `esphome config` passes (schema changes).
- [ ] `esphome compile` passes (C++ changes).
- [ ] Ran against a real ring, or explained above why not.
- [ ] `CHANGELOG.md` has an entry under `## [Unreleased]`.
- [ ] Documentation updated if observable behaviour or an option changed.
- [ ] No real MACs, serial numbers, or recording data in the diff.

## Measured versus inferred

<!--
Delete this section if the change does not touch a documented protocol or
device-behaviour claim. Otherwise: does it promote an inferred label to a
verified one, or change a stated fact? Say what evidence supports it.
-->

## On-card behaviour

<!--
Delete this section if the change does not touch what gets written to the SD
card. Otherwise: which paths, and can an incomplete transfer still be
promoted into the completed tree?
-->
