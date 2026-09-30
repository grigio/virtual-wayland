---
name: virtual-wayland
description: Run GUI tests in a nested Wayland session with its own independent virtual mouse and keyboard, so screenshots and input injection never touch the real desktop. Use when the user wants to move a mouse, press keys, click or take screenshots without disturbing their session, replace ydotool/xdotool/wtype/uinput/ctl.c, drive an app in an isolated nested compositor, or replay a ydotool+grim workflow safely. Triggers on "virtual mouse", "nested wayland", "inject input", "screenshot the test window", "don't move my mouse", "isolated GUI test session".
---

## What this does

Starts a **second `labwc` compositor** — windowed (a `virtual-wayland`
window on the real desktop) or `--headless` (no window) — and drives it
with input devices that exist **only inside that session**
(`zwlr_virtual_pointer_v1` / `zwp_virtual_keyboard_v1`).

- Screenshots come from the **nested** output, not the desktop.
- Pointer/key events go to the nested compositor **only**.
- **`/dev/uinput` is never opened, `ydotoold` is never used** — the real
  mouse and keyboard never move. Proof:
  `strace -f -e trace=openat,connect vw input mousemove -a -x 5 -y 5`
  shows only `connect("/run/user/1000/wayland-N")`.
- Safety rail: while a session runs, `vwinput` **refuses** to inject into
  any other display. Bypass with `VWINPUT_ALLOW_ANY=1` if you mean it.
- Default size = **50% of the host screen** (780×520 on this laptop),
  default scale 0.5 — the window never covers the real desktop.

## Quick start

```bash
VW=~/.pi/agent/skills/virtual-wayland/scripts/vw   # this skill's directory
$VW start --headless                # or windowed, to watch what happens
eval $($VW env)                     # export WAYLAND_DISPLAY=wayland-N

$VW run ./my-app                    # launch inside the session
$VW shot /tmp/a.png                 # screenshot of the nested output
$VW input mousemove -a -x 600 -y 500
$VW input click 0xC0                # left click
$VW input key 125:1 103:1 103:0 125:0   # Super+Up (one call = one device)
$VW shot /tmp/b.png

$VW stop                            # kills the compositor + its clients
```

`vw input` builds `bin/vwinput` on first use (`scripts/build.sh`:
wayland-scanner + cc + libwayland + libxkbcommon).

## Commands

| command | what it does |
|---|---|
| `vw start [--size WxH] [--scale N] [--headless] [--title T]` | start (default size = 50% of host screen, default scale 0.5) |
| `vw stop [--keep-clients]` | kill the compositor and (by default) its clients |
| `vw restart [--size WxH] [--scale N] [--headless]` | clean restart — the safe way to change size/scale |
| `vw status` | pid, socket, mode, scale, clients, size/scale drift warnings |
| `vw env` | prints `export WAYLAND_DISPLAY=…` (use `eval $($VW env)`) |
| `vw size WxH` | resize the output; probes for a frame, **rolls back** if the host window refuses |
| `vw scale N` | output scale (default 0.5); logical desktop = mode / scale |
| `vw shot FILE [grim args…]` | screenshot, `grim -c` (cursor included), 2×10 s timeout |
| `vw input <vwinput args…>` | inject pointer/keyboard events |
| `vw run [-f] CMD…` | run a program inside the session (`-f` = foreground); log at `/tmp/virtual-wayland/client-N.log` |
| `vw logs [N]` | tail of the compositor log |

Runtime state lives in `/tmp/virtual-wayland/` (pidfile, socket, log,
per-client logs, copied labwc config).

## Coordinates and scale — screenshot px == `-a` coordinates, always

The output scale **never breaks the mapping** (verified at 0.5 and 1.0):

- `vw shot` pins grim's `-s` to the real output scale ⇒ the PNG is exactly
  the **mode (buffer) size**.
- `vwinput -a` (and `drag`) send the **mode size** as motion extent ⇒ the
  compositor normalises over the same frame.
- ⇒ screenshot pixel (x, y) == `vw input mousemove -a -x x -y y`, **1:1 at
  any scale**. Never divide.

What scale *does* change is the logical desktop the **app** sees:
`logical = mode / scale`. Default 780×520 @ 0.5 ⇒ apps render on a
1560×1040 desktop. `vw scale N` never resizes the host window, is safe
mid-test, and cannot break the 1:1 rule. `vw status` warns when the actual
size/scale drifted from what the session was started with.

(On the *host* desktop this is different: eDP-1 is 3120×2080 @ scale 2.0,
where grim pixels must be divided by 2. Inside this session: no division.)

## vwinput — ydotool-compatible CLI

```text
vwinput [-d <display>] <command> [args]

mousemove [-a] -x <n> -y <n>   -a = absolute, native px of the output
                               (= screenshot px); default: relative
mousemove -w -y <n>            wheel, <n> notches (negative = down)
click <hex> [-r n]             0x00..0x07 button, 0x40 down, 0x80 up
                               0xC0 = left click, 0x41 = right down
                               0x00 is a no-op (ydotool quirk)
key [-d <ms>] <code>:<state>…  raw evdev keycodes, whole sequence in ONE call
scroll -x <n> -y <n>           axis units, ~15 per wheel notch
drag -x x0 -y y0 -X x1 -Y y1   absolute native px: go to (x0,y0), press,
                               walk to (x1,y1), release — all on ONE device
                               [-n steps] [-d ms], defaults 40 / 50
```

Common evdev keycodes: `Esc 1`, `Enter 28`, `Space 57`, `F1 59`, `F10 88`,
`F11 87`, `Up 103`, `Down 108`, `Left 105`, `Right 106`, `Super 125`,
`Backspace 14`, `Delete 111`.

Notes:

- **One invocation = one virtual device.** Send press/release pairs of a
  chord in a single `key` call — split calls land on different devices and
  the compositor never sees the chord.
- Targets `$WAYLAND_DISPLAY` (or `-d`); needs
  `zwlr_virtual_pointer_manager_v1` (labwc, sway, river, cage, …) and
  `zwp_virtual_keyboard_manager_v1` for `key`.

## Windowed vs headless

- **Windowed (default)**: a real window you can watch. It only renders
  while visible: hidden, occluded or hand-resized ⇒ frames stop and
  `vw shot` times out with a message (never hangs). `vw status` warns on
  size drift; `vw size` probes after resizing and rolls back if refused.
- **`--headless`**: no window, zero intrusion, screenshots return
  instantly — use for unattended runs:
  `$VW restart --headless [--size WxH] [--scale N]`.

## Translating a ydotool + grim workflow

Mapping: `ydotool mousemove` → `vw input mousemove`, `ydotool key` →
`vw input key`, `ydotool click` → `vw input click`, `grim -c` → `vw shot`,
`wtype` → `vw input key`. Host-side steps (`pkill`, app logs, `grep`) are
unchanged:

```bash
VW=~/.pi/agent/skills/virtual-wayland/scripts/vw
pkill -x carosello                      # host-side: -x matches name only
$VW start --headless
eval $($VW env)
$VW run sh -c 'CAROSELLO_DEBUG=1 /usr/bin/carosello >/tmp/carosello.log 2>&1'
$VW shot /tmp/a.png
$VW input mousemove -a -x 576 -y 23    # PNG coordinates == input coordinates
$VW input click 0xC0
$VW shot /tmp/b.png
$VW stop
```

The app's own log is a normal file, so `grep -a panel-zone /tmp/carosello.log`
and `pgrep -cx carosello` work unchanged.

## Safety rules

1. **Never inject into the host session** — always `vw input`; a bare
   `vwinput` against `wayland-0` is refused while a session runs.
2. **Never run `ydotool`/`xdotool`/`wtype` on the host** as part of a
   workflow meant for this skill — that moves the user's real pointer.
3. `pkill -x <name>` only (never `pkill -f <path>`): `-f` can match your
   own shell's command line and kill the test script.
4. The nested session gets a **stub autostart**, so kanshi/noctalia/clipman
   from `~/.config/labwc/autostart` don't start twice, while `rc.xml` /
   `menu.xml` / `themerc` are copied — `Super+Up` still means ToggleMaximize.
5. `vw restart` after `eval $($VW env)` is safe: the script remembers the
   real host socket and nests there instead of into its own dying socket.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `vw: shot timed out` / `grim timed out` | nested window not visible or resizing → bring it up, or `vw restart --headless` |
| `vw: refusing to inject into 'wayland-0'` | safety rail: use `vw input`, or `eval $(vw env)` |
| `note: … output is now …` in `vw status` | host window resized; `vw restart --size WxH` to restore |
| `note: … scale …` in `vw status` | scale changed outside vw; `vw scale N` to restore |
| nothing in the screenshot | client didn't start — `vw logs`, `vw status` (lists clients), `/tmp/virtual-wayland/client-N.log` |
| build errors on first `vw input` | `scripts/build.sh`; needs wayland-scanner, a C compiler, libwayland, libxkbcommon |
