# Desktop: a nested compositor in an anland window

anland 6 gives every Wayland toplevel its own Android window. That is ideal
for single-app containers, and it is also exactly what a desktop compositor
needs to run *nested*: KWin, Weston, Sway or Mutter already know how to be a
Wayland client of another compositor (`--wayland-display`, `--backend=wayland`,
`WLR_BACKENDS=wayland`), and the host it connects to is anland.

So "running KDE on the phone" does not need a KWin backend plugin (that was
the 5.x design: `backend-anland` re-implemented per KWin release, which broke
every time KWin changed its backend API). It needs a host that behaves like a
compositor well enough for a client compositor to trust it. This document is
about that host side, plus the launcher that starts the nested session.

```
  Android Activity (fullscreen Surface)
        ▲  eglSwapBuffers / SurfaceControl
        │
  waylandbridge  ── wl_compositor, wl_shm, linux-dmabuf(v3, LINEAR),
        │            xdg_wm_base, wl_seat, wl_output(v4), zxdg_output_v1,
        │            wp_presentation, wl_subcompositor, wl_data_device
        │
        │  one toplevel = the whole desktop
        ▼
  KWin / Weston / Sway  (nested backend: GBM + dmabuf, shm fallback)
        │
        ▼
  guest apps (Wayland and, through the compositor's own Xwayland, X11)
```

## What the host advertises

| Protocol | Why a nested compositor needs it |
| --- | --- |
| `wl_compositor`, `wl_shm` | client-side buffers; the only path when GBM/EGL cannot import dmabufs |
| `zwp_linux_dmabuf_v1` v3 | GPU buffers, with the format + modifier list it may allocate from (this build: `ARGB8888`/`XRGB8888` × `LINEAR`, `INVALID`) |
| `xdg_wm_base`, `wl_subcompositor` | toplevel roles, popups, subsurfaces (desktop shell, tooltips) |
| `wl_seat` | keyboard (XKB keymap + modifiers), pointer (motion/buttons/axis/frame), touch |
| `wl_output` v4 | the virtual screen: geometry, mode, scale, **name** and **description** |
| `zxdg_output_manager_v1` v3 | logical position/size + output name for Qt/KWin/GTK output models |
| `wp_presentation` v1 | real "frame N was presented at T, refresh R, sequence S" feedback |
| `wl_data_device_manager` | clipboard + drag and drop between guest apps |
| `zwp_linux_explicit_synchronization_v1` | acquire/release fences, when a client prefers them over frame callbacks |

`wl_output` is grow-only by design (`awl_output_grow`): it never shrinks, so a
desktop that was once shown at 2400×1080 keeps that resolution even if the
window is later shown smaller. That is what Xwayland's X screen and most
output models expect.

### presentation-time and the "mailbox" problem

A compositor paces itself with two signals: the frame callback (start the next
frame) and, if available, the presentation feedback (the frame you already
submitted is on screen now). With only frame callbacks, the compositor has to
assume "callback fired ⇒ previous frame landed", which is wrong as soon as the
host's present path is asynchronous — the KDE mailbox submission model then
queues frames that are already stale.

`wp_presentation` closes that gap: `surface_presented()` (the render thread,
right after `eglSwapBuffers`) answers every queued feedback object for that
window with the `CLOCK_MONOTONIC` timestamp of the swap, the output's refresh
period, and an incrementing sequence number. Feedback objects for a surface
that dies answer `discarded` instead, so a compositor tears its loop down
instead of waiting forever.

The timestamp is the swap return, not the panel's vsync: anland renders into
an Activity Surface, and `EGL_ANDROID_get_frame_timestamps` is not available on
this path (`SurfaceControl` never produces a frame id). Treat it as "presented
now ± one frame" — enough for pacing, not for A/V sync measurements.

## Turning it on

Both keys are also on the phone, in **Window Settings → Desktop**: the session
switch and a text field for the title list (applied over binder, written back to
`config.json`, effective on the next configure of a matching window). The file
is still the way to set them before first boot:

```json
{
  "sc_enabled": 0,
  "desktop_mode": 1,
  "desktop_match": "kwin,plasma,weston,sway,mutter,gnome-shell,wayfire"
}
```

`/data/adb/modules/anland-awl/config.json`, then restart the daemon (or set
the keys over the binder `cfg_set` / `cfg_set_str` path — `desktop_mode` applies
live, `desktop_match` re-evaluates the windows that are already open).

| Key | Range | Meaning |
| --- | --- | --- |
| `desktop_mode` | 0–1 | 1 = the toplevel whose title matches `desktop_match` is a whole-screen session |
| `desktop_match` | string | comma-separated, case-insensitive title substrings |

`sc_enabled` must stay `0`: the SurfaceControl backend hands each window to
SurfaceFlinger as its own layer, so the daemon never touches those pixels. The
GL backend is what renders into an Activity Surface, and therefore the only
backend where a nested compositor's single window can be composited, paced and
(e.g. in VR) reprojected.

What `desktop_mode` does when a window matches:

* every `xdg_toplevel.configure` for it also carries
  `FULLSCREEN | MAXIMIZED` (see `awl_xdg.c`), so the compositor stops treating
  the host as a floating WM;
* the window is pinned to the panel size (`awl_window_resize` to the physical
  panel) and the `wl_output` mode is grown to cover it;
* the hosting Activity is asked to keep the screen on — a locked screen kills
  the guest's frame loop.

## Running it

Inside the container, from the anland session:

```sh
bash anlandx/anland-session.sh &        # Xwayland + mini-wm + D-Bus, ~/.anlandx-env
bash anlandx/anland-desktop             # auto-detects kwin_wayland > weston > sway > mutter
bash anlandx/anland-desktop --compositor weston    # explicit
bash anlandx/anland-desktop --print-env           # just print the session env
```

`anland-desktop` sources `~/.anlandx-env`, gives the nested session its own
`XDG_RUNTIME_DIR` (`$XDG_RUNTIME_DIR/anland-desktop`) and its own Wayland
socket (`wayland-desktop`), and writes `~/.anlanddesk-env` so apps can be
started inside the desktop:

```sh
. ~/.anlanddesk-env
wayland-info | head                 # confirm the host's globals
kate &                              # a Qt app on the nested desktop
```

Compositor installs inside the container (Arch shown; Debian/Ubuntu names in
parentheses):

```sh
pacman -S weston                    # ubuntu: weston            (smallest, smoke test)
pacman -S sway                      # ubuntu: sway              (wlroots)
pacman -S kwin plasma-workspace     # ubuntu: kwin-wayland, plasma-workspace
```

Weston is the right first test: it needs nothing but EGL/GLES and reports
clearly whether the host's dmabuf path or the shm path is in use. KWin is the
target experience; it additionally wants Qt 6, KF6 and a session D-Bus.

### If GPU import fails

`zwp_linux_dmabuf_v1` here advertises only `LINEAR`, and the host has to turn
each client dmabuf into an `AHardwareBuffer` before EGL can sample it
(`awl_ahb.cpp`). On some Adreno/Turnip combinations a tiled buffer arrives
anyway and import fails; the compositor then falls back to `wl_shm` on its own
(KWin: `KWIN_COMPOSE=Q`, Sway: `WLR_RENDERER=pixman`, Weston:
`--use-pixman`). That works, but every frame is a CPU copy — fine for
diagnosis, too slow for daily use.

## Verifying a session

```sh
# 1. the daemon saw the window as a session
logcat -s waylandbridge | grep -i 'desktop session'
#    window 7: desktop session on (Plasma Desktop), pinned to 1832x1920

# 2. the host globals a compositor expects
. ~/.anlanddesk-env
wayland-info | grep -E 'wl_output|zxdg_output|wp_presentation|linux_dmabuf'
#    wp_presentation (v1), zxdg_output_manager_v1 (v3), zwp_linux_dmabuf_v1 (v3)

# 3. output geometry matches the window
wayland-info | grep -A3 'interface: zxdg_output_v1'
#    logical_size: 1832x1920      <- not 800x600, not the old window size

# 4. presented feedback is flowing (KWin: KWIN_DEBUG=1, weston: WAYLAND_DEBUG=1)
```

`wp_presentation` `presented` events with sane timestamps mean the render loop
is paced by the host rather than by a guess. `discarded` events at shutdown are
normal.

## Known limits

* **No DRM/KMS, no direct scanout.** The desktop is one guest window; the host
  (SurfaceFlinger) is always the final compositor. Nothing can be scanned out
  from the guest.
* **One desktop at a time.** The session mark is per window, and there is one
  panel: a second matching window is pinned too, but only one of them is
  visible.
* **`LINEAR` only.** Tiled/compressed guest buffers must be imported by the
  host; when that fails, compositors drop to `wl_shm`.
* **No hardware cursor plane from the guest.** The compositor draws its own
  cursor into its buffers (host-side cursor layers exist for ordinary
  windows).
* **Frame pacing is host-side.** The host presents when the window's buffer
  arrives and the panel vblanks; a compositor that wants strict pacing should
  honour `wp_presentation` rather than assume a fixed refresh.
* **3DoF only in VR.** Running a desktop inside a VR session inherits the
  limits of `docs/vr.md` (orientation-only head tracking, one XR window).
* **Not device-tested yet.** The host protocols, the session mode and the
  launcher compile and are self-consistent; a nested KWin/Plasma session has
  not been run on the author's hardware. The verification steps above are the
  ones to follow on a real device, and `logcat -s waylandbridge` is where the
  host side explains itself.
