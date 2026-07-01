# Native multitouch (WM_TOUCH / WM_POINTER) support

This branch adds a genuine absolute multitouch input path so Windows games
running under GameNative can receive real multi-point touch input
(`WM_POINTERDOWN/UPDATE/UP`, and `WM_TOUCH` for `RegisterTouchWindow()`
clients) instead of the existing single-cursor mouse emulation.

Pipeline: Android `MotionEvent` (per-pointer) → GameNative X server
(XInput2 **raw touch** events) → wine `winex11.drv` (already implemented
upstream of this change) → `WM_POINTER` / `WM_TOUCH`.

## Why raw touch events, not XI_TouchBegin

The XInput2 protocol has two touch delivery mechanisms: per-window touch
events (`XI_TouchBegin/Update/End`, with grabs and ownership arbitration) and
root-window raw events (`XI_RawTouchBegin/Update/End`).

Reading the winex11.drv source in the bundled proton-wine fork
(`GameNative/proton-wine`, both `proton_9.0` and `bleeding-edge`) shows wine
**only consumes the raw variant**: the desktop-window thread (WinSta0) sets
`xinput2_rawinput = TRUE` and selects `XI_RawTouchBegin/Update/End` on the
root window for `XIAllMasterDevices` (`x11drv_xinput2_enable`), and
`X11DRV_RawTouchEvent` translates them into `WM_POINTER*` hardware input.
`XI_TouchBegin` is never selected. Implementing per-window touch would
therefore do nothing for wine; implementing raw touch is both correct and
substantially simpler (no grabs, no ownership, no per-window routing).

Hard requirements extracted from wine's `map_raw_event_coords` /
`update_relative_valuators` / `X11DRV_RawTouchEvent`:

1. Raw touch events must carry `deviceid` equal to the master pointer id
   returned by `XIGetClientPointer` (2 in this server).
2. The master pointer's X/Y valuator classes (from `XIQueryDevice`, re-read on
   `XI_DeviceChanged`) must both be **mode = Absolute** with a real
   `min`/`max` range; wine scales `(value - min) / (max - min)` onto the
   0–65535 virtual desktop. Events on a relative-mode device are rejected for
   touch (`MOUSEEVENTF_ABSOLUTE` check).
3. Every event must include both valuators (mask bits 0|1) with
   `values`/`raw_values` as FP3232.
4. `detail` carries the touch sequence id (becomes the Windows pointer id).

All wire formats were taken from `XI2proto.h` / `XIproto.h`, not inferred.

## What was added

**X server (`com.winlator.xserver`)**

- `events/XIRawTouchNotify.java` (new): `xXIRawEvent` XGE serialization for
  evtypes 22/23/24, same layout as the existing `XIRawMotionNotify`.
- `events/XIDeviceChangedNotify.java` (new): `xXIDeviceChangedEvent` so wine
  re-reads valuator classes when the mode is toggled at runtime.
- `XInput2Extension.java`:
  - `emitRawTouch()` / `emitDeviceChanged()`, delivered per the existing
    `XISelectEvents` selection tracking (mask bit = `1 << evtype`).
  - `XIQueryDevice` now advertises the X/Y valuators as Absolute with
    `max = screen dimension - 1` when native touch mode is on (unchanged
    Relative/0 otherwise). Class serialization was factored into static
    helpers shared with the DeviceChanged event.
  - Minimal well-formed replies for the XI 1.x requests wine and stock libXi
    actually issue (`ListInputDevices`, `OpenDevice`, `CloseDevice`,
    `GetDeviceButtonMapping` — wine's `update_device_mapping()` calls the
    latter three with the sourceid of every raw button event). These
    previously answered `BadImplementation`.
- `XServer.java`:
  - `injectTouchBegin/Update/End(touchId, x, y)` API.
  - New constructor overload carrying the container's native-touch flag; the
    old signature delegates with `false`.
  - XInput2 registration condition relaxed from `!runningFromGlibc` to
    `!runningFromGlibc || nativeTouchMode` (see "glibc" below).
  - In native touch mode, raw motion for the mouse paths is emitted as
    absolute positions instead of deltas, matching the Absolute valuator
    advertisement (wine requires both axes to share one mode for all raw
    events, touch and motion alike).

**Android input (`com.winlator.widget.TouchpadView`)**

- New `nativeTouchMode` routed in `onTouchEvent()` after the stylus check and
  before the touchscreen/touchpad mouse paths. Each Android pointer id maps to
  a monotonically increasing X touch id for the lifetime of its sequence
  (`ACTION_DOWN`/`POINTER_DOWN` → `MOVE`s → `POINTER_UP`/`UP`;
  `ACTION_CANCEL` ends all active sequences, since XI2 raw touch has no
  cancel). Coordinates go through the same `XForm` view→X-screen transform as
  the mouse paths. No-movement `MOVE`s are deduplicated. External mice
  (`SOURCE_MOUSE`) and styluses keep their existing behavior in this mode.

**Settings/UI**

- `Container.nativeTouchMode` (persisted, default `false`).
- Quick Menu → Controller tab: "Native Touch (Multitouch)" toggle. Toggling
  at runtime works when the XInput2 extension is registered (emits
  `XI_DeviceChanged` so wine flips valuator modes live); on a glibc container
  started with the flag off, a toast explains it takes effect next launch.

## What was NOT touched

- The existing touchpad-mouse and touchscreen-cursor modes, stylus handling,
  and the core-protocol `MotionNotify`/`ButtonPress` path are unmodified and
  remain the default. With the flag off, the only behavioral delta anywhere is
  the four XI1 opcodes now answering minimal replies instead of
  `BadImplementation` (and those were previously only reachable on bionic).
- wine/proton: zero changes needed; the consuming side already exists.

## The glibc question (main risk)

`setupExtensions()` previously registered XInput2 only for non-glibc
containers — deliberately, but the shallow history doesn't show why
(plausibly: stock libXi in the glibc rootfs probing requests that the server
answered with `BadImplementation`). Since Proton runs in glibc containers,
native touch requires lifting that gate, which this branch does **only when
the user enables native touch for that container** — default-off, so existing
glibc setups are unaffected.

Must be verified on-device for glibc containers with the toggle on:

1. `libXi.so` exists in the glibc rootfs (wine dlopens `SONAME_LIBXI`; without
   it `xinput2_available` stays false and nothing works). Check
   `proton-9.0-x86_64_container_pattern.tzst` / the imagefs.
2. wine boots cleanly with XInput2 advertised (watch for hangs/errors during
   explorer/desktop startup; `WINEDEBUG=+xinput2` traces
   `XInput2 2.2 available` and per-event lines).
3. If breakage reproduces, capture which request precedes it — the XI1
   hardening in this branch is the most likely fix, but the original failure
   mode is unconfirmed.

## Verified vs. assumed

Verified by reading source:
- wine raw-touch consumption, event selection, deviceid/valuator-mode
  requirements (GameNative/proton-wine `proton_9.0` + `bleeding-edge`,
  `dlls/winex11.drv/{mouse,window,event}.c`).
- `proton_9.0` additionally synthesizes left-mouse from the *primary* touch
  (Steam-Deck style), so on that branch touch mode also produces mouse input;
  `bleeding-edge` emits `WM_POINTER` only.
- All X wire formats against `XI2proto.h` / `XIproto.h`.
- This fork's win32u does **not** implement `SM_DIGITIZER`
  (`GetSystemMetrics` returns 0). Games that gate touch handling on it will
  ignore touch even with a working pipeline; that would need a small wine
  patch and is out of scope here.

Assumed / to verify on device:
- libXi presence in the glibc rootfs; glibc boot behavior with XI2 on.
- Which proton-wine branch a given shipped build corresponds to (both handle
  raw touch, so the pipeline works either way; only the touch→mouse synthesis
  differs).

## Testing

1. Build `tools/touchtest/touchtest.c`
   (`x86_64-w64-mingw32-gcc -O2 -D_WIN32_WINNT=0x0602 touchtest.c -o touchtest.exe -lgdi32 -luser32`)
   and run it in a container with Native Touch enabled. Expect: per-finger
   `WM_POINTER` logs with stable ids, `WM_TOUCH` after `RegisterTouchWindow`,
   one colored circle per finger, 10 simultaneous contacts tracked.
2. Regression: with the toggle off, verify touchpad mode, touchscreen mode,
   stylus, external mouse, and (on glibc) that nothing changed at all.
3. Real game: any WM_TOUCH/WM_POINTER consumer.

### Note on arcade titles with IO-hook DLLs (e.g. Crossbeats REV)

Arcade ports driven by community IO hooks may not read Windows touch messages
at all — some hooks emulate the cabinet's touch panel via its serial/USB
protocol or raw HID instead. Before concluding the pipeline is broken for such
a title, check the hook: `objdump -p hook.dll | grep -iE "RegisterTouchWindow|GetPointer|GetTouchInput"`
(imports present → it consumes standard touch and this feature applies;
absent → the hook has its own input path and needs its own configuration).
touchtest.exe working while the game doesn't react points to the hook, not
this plumbing.
