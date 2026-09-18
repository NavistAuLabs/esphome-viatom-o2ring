# esphome-viatom-o2ring

An [ESPHome](https://esphome.io) external component that pulls stored
recordings off a **Wellue/Viatom O2Ring** pulse oximeter over BLE and writes
them to an SD card on the ESP32 doing the pulling. No cloud, no vendor app,
no account. Files land byte-identical to what the ring serves.

The ring is a plain unauthenticated GATT peer: no bonding, no encryption, no
per-device secret. This component speaks its file-transfer protocol directly.

Protocol notes, the corrected `.vld` layout and the implementation traps live
in
[`components/viatom_o2ring/README.md`](components/viatom_o2ring/README.md).

## Read this before you build anything around it

The ring's behaviour, not the code, is what decides whether a sync ever
happens. Four facts do most of the work:

- **It is only reachable while worn or while charging.** Asleep, it is off
  the air entirely — not quiet-but-connectable. There is no way to wake it
  over the radio.
- **A dock is not automatically a window.** A discharged ring stays awake for
  the whole charge (16+ minutes observed); a ring that was already full wakes,
  finds nothing to do, and sleeps again within a couple of minutes.
- **The session being recorded is never downloadable.** The recording is
  written and added to the ring's file list only after it comes off and
  finishes a roughly 10-second countdown. Wearing it gives you an indefinite
  *connection* window that never contains the night you are currently
  recording.
- **Four storage slots, and the oldest is evicted when a session STARTS** —
  when the ring goes back on, not when a recording saves. **A sync has to land
  between wears or the oldest recording is gone.** That is the constraint this
  component is built around, and it is why it walks the whole file list
  oldest-first rather than grabbing the newest one.

Put together, the usable sequence is:

```
wear overnight        ring advertises, but the live session is NOT listed
take it off           ~10s countdown, then the recording is written + listed
dock it (discharged)  awake for the whole charge -> this is the window
fully charged         sleeps, off the air entirely
```

So the component does not poll on a timer. It watches for the ring's
advertisements going from absent to present — which, after a night's wear,
means it was just docked — and starts a sync on that edge.
`min_sync_interval` is only a floor between attempts so that one long charge
does not cause repeated reconnects; it is not a schedule.

## Installation

### Requirements

- **A board within BLE range of wherever the ring charges.** Developed and
  verified on the **ESP-IDF** framework.
- **`esp32_ble_tracker`** — the component registers as an advertisement
  listener, which is how it notices the ring waking up.
- **`ble_client`** for the ring, with **`auto_connect: false`**. The component
  opens and closes the link itself, and that matters: a connected BLE
  peripheral stops advertising, so a link held open makes the ring invisible
  to every later scan.
- **An SD card mounted at `/sdcard`** (`sd_mmc_card`'s mount point). Without
  a card there is nowhere for a recording to land.
- **A recent ESPHome.** Older versions fail the build on APIs this component
  uses; there is no compatibility shim.

### 1. Find the ring's MAC

Put the ring on, so it advertises, then run an ESPHome BLE scan and look for
the address that appears while you are wearing it. The advertisement carries no
service UUIDs, so the MAC is the only way to identify it.

### 2. Add the component to your device configuration

```yaml
external_components:
  - source: github://NavistAuLabs/esphome-viatom-o2ring@v0.1.0
    components: [viatom_o2ring]
  # SD card mount. Pinned at the commit upstream's v0.2.0 tag points at: a
  # branch moves, and a tag can be repointed.
  - source: github://n-serrette/esphome_sd_card@889073e052275aeb9e826b697efe3bbbe09f935d
    components: [sd_mmc_card]

esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      # FATFS long filenames are off by default, which caps every path at 8.3
      # and fails the timestamped recording filenames with EINVAL.
      CONFIG_FATFS_LFN_HEAP: "y"
      CONFIG_FATFS_MAX_LFN: "255"
    advanced:
      # Since ESPHome 2026.2.0 the built-in ESP-IDF components are excluded
      # from the build, and sd_mmc_card needs fatfs plus these VFS features.
      include_builtin_idf_components: [fatfs]
      disable_vfs_support_termios: false
      disable_vfs_support_select: false
      disable_vfs_support_dir: false

esp32_ble_tracker:

sd_mmc_card:
  id: sd_card
  # Pins are board-specific -- these are an Olimex ESP32-POE-ISO wired for
  # 1-bit mode. Use your own board's SD pinout.
  mode_1bit: true
  clk_pin: GPIO14
  cmd_pin: GPIO15
  data0_pin: GPIO2
  # Auto-formatting on a mount glitch would destroy recordings the ring has
  # already rotated away.
  format_if_mount_failed: false

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"   # your ring's MAC, from step 1
    id: o2ring
    auto_connect: false                # the component drives the connection

viatom_o2ring:
  ble_client_id: o2ring
  min_sync_interval: 30min
  battery_level:  { name: "O2Ring Battery" }
  serial_number:  { name: "O2Ring Serial" }
  state:          { name: "O2Ring State" }
  file_list:      { name: "O2Ring File List" }
  last_sync:      { name: "O2Ring Last Sync" }
```

### 3. Confirm a sync

Wear the ring overnight, take it off, and dock it discharged. Set
`logger: level: VERBOSE` for the first run: the component logs every
advertisement it sees at that level, which is how you tell "the listener is
not being called" apart from "the ring is asleep". A completed walk logs
`FileList walk finished clean` and leaves files under
`/sdcard/o2ring/<serial>/`.

## Configuration

| Option | Type | Default | Meaning |
|---|---|---|---|
| `ble_client_id` | id | the single `ble_client` | Which `ble_client` peer is the ring. Name it explicitly if the board has more than one. |
| `min_sync_interval` | time period | `30min` | Floor between sync attempts. Stops a long continuous advertising period, such as a full charge cycle, from reconnecting over and over. Not a schedule — the advertisement edge is what decides *when*. |
| `battery_level` | sensor | — | Ring battery percentage, from the device's `CurBAT`. Diagnostic. |
| `serial_number` | text sensor | — | The ring's serial, from `SN`. Also the directory name recordings are filed under. |
| `state` | text sensor | — | The ring's raw `CurState`, published as reported rather than decoded. `0` and `2` are the values seen — awake, and post-session standby. |
| `file_list` | text sensor | — | The ring's file list as of the last connection — the authoritative list of what it is actually holding. |
| `last_sync` | text sensor | — | The ring's own clock (`CurTIME`) from the last walk that finished with **no** failed transfer. See below. |

All five entities are optional; configure only the ones you want.

`last_sync` is deliberately the ring's clock rather than the board's uptime,
and it only advances on a clean walk. That distinction is what lets you tell
"nobody wore the ring, nothing new to fetch" (the walk still finishes clean,
`last_sync` advances) apart from "there was something and we failed to get
it" (`last_sync` stops advancing). Alert on `last_sync` going stale, not on
the absence of new files.

## What lands on the card

```
/sdcard/o2ring/<serial>/<name>.vld            completed recordings
/sdcard/.partial/o2ring/<serial>/<name>.vld   transfer in progress
```

A file is only moved into place once the bytes written match the size the
ring declared when the file was opened. Anything short — a disconnect
mid-transfer, a write failure — is left in `.partial/` rather than promoted.
A truncated recording that looks complete is worse than an obviously
incomplete one, because anything downstream would publish it as real data.

Files already present under the completed path are skipped on the next walk,
so a sync that is interrupted resumes the remaining files next time the ring
wakes up rather than re-downloading everything.

## Troubleshooting

**Nothing happens at all.** The ring is almost certainly asleep. It is only
on the air while worn or charging. Set `logger: level: VERBOSE` and watch for
`advertisement seen` — that line is this component's own, because
`esp32_ble_tracker` logs nothing per advertisement and its silence is
indistinguishable from a listener that is never called.

**Connects fail with `reason 0x100` then `status=133`.** Expected, and
frequent — roughly half of attempts, after a 20-second timeout. The
connection is being cancelled locally rather than refused by the ring. The
component retries up to 4 times, 3 seconds apart, which is comfortably inside
the minutes-long window a charging ring stays awake.

**`Edge ignored: client state N, not IDLE`.** Something else is holding the
BLE client — most often `auto_connect: true` on the `ble_client`. The sync
trigger is a one-shot edge, so while the client is stuck non-IDLE every later
edge is swallowed until the ring next goes fully off the air.

**The ring never appears while it is in the charger.** Check that it was
actually discharged when docked. A full ring's window is a couple of minutes,
which can easily be missed.

**Recordings have vanished from the ring.** Four slots, evicted at the start
of the next session. If syncs are not landing between wears, the oldest
recording is lost before anything can fetch it.

**`sd_mmc_card` fails validation.** Its `id:` cannot be `sd_mmc_card` — that
collides with the integration's own name.

**Files stay in `.partial/`.** The transfer did not reach the declared size.
The log line names the byte counts; the usual cause is the ring going to
sleep mid-walk, which resolves itself on the next wake because completed
files are skipped.

## Interoperability and scope

This is an independent, clean-room-from-observation implementation of a
protocol spoken by hardware you own, written so that data a device records
about you can be read off it without a vendor cloud account. It is not
affiliated with, endorsed by, or supported by Viatom, Wellue, or any
reseller; product names are used only to identify the hardware it talks to.

It reads recordings the ring has already stored. It does not modify firmware,
change device settings, or unlock anything.

Not a medical device, and nothing it produces is medical advice. The `.vld`
parsing notes in the component README mark clearly which fields are verified
and which labels are inherited guesses — treat the unverified ones as opaque.

## License

[PolyForm Noncommercial License 1.0.0](LICENSE). The copyright holder and the
required notice are named at the top of [LICENSE](LICENSE).

Plainly: this is **source-available, not open source**. It is not an
OSI-approved license. Personal use, hobby projects, research, experiment and
testing are permitted purposes, as is use by charities, educational
institutions, public research or health organisations, and government
institutions. **Commercial use is not licensed.** If you want to build
something commercial on it, ask.

If you redistribute any part of it, you must pass on the license terms (or
their URL) and the `Required Notice:` line at the top of [LICENSE](LICENSE).

## Contributing

Issues and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md)
for what a change needs, and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).
Security reports go through the process in [SECURITY.md](SECURITY.md), not a
public issue.
