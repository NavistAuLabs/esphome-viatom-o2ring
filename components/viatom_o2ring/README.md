# viatom_o2ring — how it works

Protocol and implementation reference for the `viatom_o2ring` component:
the Viatom BLE protocol against a **Wellue O2Ring** pulse oximeter,
recordings pulled byte-identical to what the device serves. For installing
and using the component — full device configuration, SD card mount,
troubleshooting — see the root [README](../../README.md); nothing here is
needed to run it.

Recordings land at `/sdcard/o2ring/<serial>/<name>.vld`, promoted from
`.partial/` only once complete.

## Protocol

No bonding, no encryption, no secret — the ring is open.

```
service 14839ac4-7d7e-415c-9a42-167340cf2339
write   8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3   (write-without-response)
notify  0734594a-a8e7-4b1a-a6b1-cd5243059a57
```

```
request: AA | CMD    | CMD^0xFF    | BLOCK(2 LE) | LEN(2 LE) | DATA | CRC8
reply:   55 | STATUS | STATUS^0xFF | BLOCK(2 LE) | LEN(2 LE) | DATA | CRC8
```

**Replies are marked `0x55`, not `0xAA`** — the device answers with the
complement of the request marker — **and byte 1 is a STATUS (`0x00` = OK),
not an echo of the command.** Verified by dumping raw notifications: an
INFO reply is 26 notifications, 520 bytes = 7 header + 512 payload + 1
CRC, matching its declared length exactly, with the 492-byte JSON
zero-padded to a fixed 512.

**Do not read byte 1 as a command echo.** An implementation that does, and
that then finds the JSON by searching the buffer for `{...}` rather than
computing the payload offset from the header, appears to work on INFO and
mis-reads every other reply. It is also where the claim that `FILE_OPEN`
returns an unreliable "status" comes from — that byte was always a status,
just read as a command echo. **Implement the table above, not that
framing.**

| cmd | meaning |
|---|---|
| `0x14` | INFO — JSON with `SN`, `CurBAT`, `CurState`, `CurBatState`, `CurTIME`, `FileList` |
| `0x03` | FILE_OPEN — filename + NUL; reply carries the 32-bit size |
| `0x04` | FILE_READ — BLOCK selects the block |
| `0x05` | FILE_CLOSE |

**`FileList` is authoritative; `FILE_OPEN` lies.** It returns status 0 and
a size even for a filename it does not have, echoing the previous open's
metadata. Walk the list from INFO.

Requests are written in 20-byte fragments with a ~20 ms gap. Whether that
reflects a device limit or an untuned MTU is unknown.

## Device behaviour — measured, not inferred

**It sends nothing unsolicited.** Everything is request/response: there is
no live stream to tap and no push when a session saves. Subscribing and
listening while the ring is worn and actively recording yields nothing.

**It is invisible when asleep** — off the air entirely, not
quiet-but-connectable. It advertises while worn and while charging.

**The in-progress session is not downloadable.** The recording is only
written and added to `FileList` after the ring comes off and finishes a
~10 second countdown. Wearing it gives an indefinite *connection* window
but never yields the session being recorded.

**Four storage slots, evicted at session START** — when the ring goes on,
not when a recording saves. Putting the ring back on is what drops the
oldest recording, so a sync must land between wears.

**A full battery makes the charge window short.** Docking a discharged
ring keeps it awake for the whole charge (16+ min observed); docking a
full one gives a couple of minutes.

**The advertisement cannot distinguish worn from docked.** Manufacturer
data is `0xF34E: 00` in every state — worn, removed, during the
post-session countdown and "END", docked, and charging. No service UUIDs
are advertised. `CurBatState` (`1` charging) answers it, but only from inside
a connection.

The end-to-end capture window these facts add up to is laid out for users
in the root [README](../../README.md).

## Gotchas

**RELEASE THE LINK.** A connected BLE peripheral stops advertising, so the
ring is invisible for as long as a link is held open — including to the
board's own reconnect attempts, which then fail against a peer they are
already attached to and present as `ESP_GATT_CONN_CONN_CANCEL`. A held link
also preempts the ring's own countdown / "saving" / "END" sequence, which
risks the recording never being written at all. **Every terminal path
disconnects**, not just success. When the ring goes invisible, a link left
open is the first thing to check: `bluetooth_proxy` slot contention, scan
duty cycle and the vendor phone app all look like plausible causes and are
not it.

**Presence timeout must exceed the connect timeout.** The tracker reports
no advertisements while a connect is in flight, and the connect timeout is
20s. A presence window shorter than that expires during every failed
connect and makes the next advertisement look like a fresh wake. This
component uses 60s.

**Log at `ESP_LOGV`, never `ESP_LOGVV`.** The device logger runs at
`VERBOSE` and compiles `VV` statements out entirely, producing silence
indistinguishable from a listener that is never called.

**`esp32_ble_tracker` logs nothing per advertisement.** Without this
component's own advertisement log, "is the listener even being called?"
is unanswerable from outside the device. Keep it.

**Never return silently from a guard.** An edge is a one-shot; a
swallowed one is invisible until the ring next goes fully off the air.
`maybe_start_sync_()` says why it skipped.

**`ble_client.connect` fails roughly half the time** with
`ESP_GATTC_DISCONNECT_EVT reason 0x100` then `OPEN_EVT status=133`, after
a 20-second timeout — cancelled locally, not refused. Retry; the
component does, up to 4 times.

**`sd_mmc_card`'s `write_file`/`append_file` open and close per call.** At
~40 bytes a block that is hundreds of cycles per recording. Hold one
`FILE*` for the transfer and use POSIX I/O.

**The `sd_mmc_card` id cannot be `sd_mmc_card`** — it collides with the
integration's own name and fails validation.

## `.vld` format

The published `farolone/wellue-o2ring-protocol` offsets are **wrong**: it
documents duration as `u16 @ 18`. Offset 18 is the SpO2 *minimum*, a
small plausible-looking number that passes a glance. Verified layout:

| offset | type | field |
|---|---|---|
| 0 | u16 LE | version (3) |
| 2..8 | | start Y/M/D/H/M/S |
| 9 | u32 LE | file size |
| 13 | u32 LE | duration seconds (= records × 4) |
| 17 | u8 | mean SpO2 |
| 18 | u8 | min SpO2 |
| 40.. | 5 bytes each | record |

Records start at offset 40 with a 5-byte stride — verified exactly:
`(len − 40) / 5 == duration / 4`.

| byte | field | status |
|---|---|---|
| 0 | SpO2 | **verified** — plausible saturation percentages (e.g. 95–99) in every record |
| 1 | heart rate | **verified** — plausible pulse rates (e.g. 55–80 bpm) |
| 2 | "invalid" | label unverified — zero in every record of a whole overnight recording |
| 3 | "motion" | label unverified — varies across 0–127, plausible but farolone's name |
| 4 | "vibration" | label unverified — zero in every record of a whole overnight recording |

Treat bytes 2–4 as opaque flags until something proves their meaning.
