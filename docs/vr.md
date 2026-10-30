# VR: running SteamVR in an anland window

anland turns every Wayland toplevel into an Android window. SteamVR's
`vrcompositor` is, to the container, just another client with a window — so the
VR path here is not a display hand-over: the compositor keeps drawing into its
toplevel, and the daemon bends that image into the two eyes while it blits the
window to the phone or the AR glasses.

That reuses everything anland already does (dma-buf import, EGLImage, the
attach/detach lifecycle) and keeps SurfaceFlinger in charge of the display.
The price is that nothing outside anland can drive the headset: the pose has to
come back from Android, and SteamVR must be told to present through a window
instead of taking a DRM display over.

```
Android                          host daemon                container
┌──────────────┐  binder  ┌───────────────────────┐  wayland  ┌───────────────┐
│ AwlXrSession │ ───────► │ awl_xr.cpp            │ ◄──────── │ vrcompositor  │
│ IMU + pad    │  pose    │  • lens distortion    │  dmabuf   │ (SBS or mono) │
│              │  input   │  • timewarp          │           │               │
└──────────────┘          │  • anland-xr-display │           └───────────────┘
                          │  • anland-xr-pose    │ ────────►  driver_anland.so
                          └───────────────────────┘  seqlock   (HMD + 2 pads)
```

## What is new

| Piece | Where | Job |
| --- | --- | --- |
| `AwlXrSession` | `app/libawl` | 3DoF head pose from the IMU, gamepad axes/buttons, pushed over binder at ~250 Hz / 60 Hz |
| `awl_xr.cpp` | `services/waylandbridge` | marks XR windows, runs the distortion+timewarp pass, publishes `anland-xr-*` files |
| `anland_xr_wire.h` | `xr/include` | the shared file layout (display config, pose block, input block) |
| `driver_anland.so` | `xr/driver` | OpenVR server driver: HMD (`IVRDisplayComponent`) + two controllers |
| `libVkLayer_anland_vr.so` | `xr/layer` | implicit Vulkan layer that hides `VK_KHR_display` while an XR session is on |
| `anland-steamvr` | `xr/` | install / status / start / stop helper |

## Requirements

- anland 6.x with the daemon from this tree (the XR hooks are in
  `waylandbridge`), host APK from this tree (the XR session lives in `libawl`).
- `sc_enabled = 0`. The XR pass runs in the EGL renderer; the SC backend has no
  hook for it and will just show the raw window.
- SteamVR 2.x in the container. `xr/build.sh` builds for the container's own
  architecture, so build it *there* (aarch64 phone, or x86_64 for a desktop
  test) — the NDK cannot build these, the container is glibc.
- A Vulkan ICD that works in the container (Turnip/KGSL). The layer's whole
  reason to exist is the turnip case `VK_KHR_display` mishandles.

## Turning it on

Daemon config, `/data/adb/modules/anland-awl/config.json` (edit, then
`stop && start` the module service or reboot):

```json
{
  "sc_enabled": 0,
  "xr_enabled": 1,
  "xr_mode": 2,
  "xr_fov": 90,
  "xr_ipd": 63,
  "xr_k1": 0,
  "xr_k2": 0
}
```

All XR values are integers: percentages and lens coefficients are per-mille, so
`xr_k1 = 150` means `k1 = 0.15` in the shader.

Keys, all optional apart from `xr_enabled`:

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `xr_enabled` | `0` | 0–1 | turn the XR path on |
| `xr_mode` | `2` | 0–3 | `1` mono (one eye stretched over the panel), `2` side-by-side horizontal, `3` side-by-side vertical, `0` disables the pass |
| `xr_refresh` | `0` | 0–240 | override headset refresh (Hz); `0` = panel refresh |
| `xr_eye_w`, `xr_eye_h` | `0` | 0–4096 | per-eye render size; `0` = derived from the panel and `xr_mode` |
| `xr_fov` | `90` | 30–170 | horizontal FOV per eye in degrees; the vertical one follows the eye aspect |
| `xr_ipd` | `63` | 20–90 | IPD in millimetres |
| `xr_scale` | `1000` | 200–2000 | per-mille multiplier on the per-eye size |
| `xr_k1`, `xr_k2` | `0` | −1000–2000 | per-mille barrel distortion coefficients; `0` = no lens correction, straight pass-through |
| `xr_ca` | `0` | 0–100 | per-mille chromatic aberration compensation |
| `xr_match` | `vrcompositor,steamvr` | — | comma-separated, case-insensitive substrings that mark a window as XR |

`xr_k1`/`xr_k2` are for cardboard-style lenses; for AR glasses, or a phone
panel looked at directly, leave them at `0` and the pass becomes a plain
(then timewarped) blit.

### Meta Quest 3S

The Quest 3S is a two-panel Android device: 1832×1920 per eye, 90 Hz by
default (120 Hz available), roughly 96°×90° per eye, 63 mm nominal IPD. Run
anland on the headset itself, so the XR window *is* the display, and the panel
is seen through Fresnel lenses — which means the barrel coefficients have to be
non-zero, unlike the glasses case.

```json
{
  "sc_enabled": 0,
  "xr_enabled": 1,
  "xr_mode": 2,
  "xr_refresh": 90,
  "xr_eye_w": 0,
  "xr_eye_h": 0,
  "xr_scale": 900,
  "xr_fov": 96,
  "xr_ipd": 63,
  "xr_k1": 220,
  "xr_k2": 40,
  "xr_ca": 10
}
```

- `xr_eye_w/h = 0` derives 1832×1920 from the panel; `xr_scale = 900` renders
  slightly under native and lets the timewarp stretch do the rest — the first
  knob to turn if `vrcompositor` reports late frames.
- The `k1`/`k2` above are a starting point for the Q3S lens, not a measurement:
  look at a straight grid (SteamVR's room outline or `steamvr.vrsettings`'
  `driver_anland` test pattern) and raise `k1` until the edges stop bowing
  outward. `xr_ca` trims the colour fringing that shows up at the rim.
- Horizon OS hands raw IMU to VR apps through OpenXR, not to plain 2D
  activities. If `./anland-steamvr status` shows a `seq` that keeps counting
  while the quaternion stays at `0,0,0,1`, the headset is refusing the sensor:
  set `xr_mode = 1` and use SteamVR's seated mode, or root the device far
  enough to expose `SensorManager` to the host APK. Everything else (SBS
  output, distortion, controllers) works either way.
- Root is a hard requirement in any case: `anland` is a KernelSU/SukiSU module
  plus a binder daemon in the `awl_daemon` domain.

Then, inside the container:

```sh
tar xzf anlandx.tar.gz && bash anlandx/setupanlandx.sh   # once, as usual
cd anlandx/xr
./build.sh                    # driver + layer into xr/build/
./anland-steamvr install      # -> SteamVR/drivers/anland, ~/.local/share/vulkan
./anland-steamvr start        # vrstartup.sh with ANLAND_VR_LAYER_ENABLE=1
```

`install --system` puts the layer in `/usr/share/vulkan/implicit_layer.d`
instead of the per-user directory. `anland-steamvr status` prints what the
daemon is currently publishing, which is the first thing to look at when the
headset stays black.

## How a frame gets bent

1. `vrcompositor` creates its toplevel; `cb_window_title()` sees a title that
   matches `xr_match`, so the window is flagged XR.
2. The daemon sends `AWL_C_XR` (mode, refresh, eye size, FOV, IPD) through the
   window's control binder. `AwlWindowActivity` starts `AwlXrSession`.
3. Every IMU sample goes back as `AWL_T_XR_POSE`, gamepad state as
   `AWL_T_XR_INPUT`. The daemon keeps the latest pose in a seqlock and mirrors it
   to `$ANLAND_RUNTIME_DIR/anland-xr-pose`, inputs to `anland-xr-input`.
4. The driver's `RunFrame()` reads those files and feeds OpenVR. Its
   `IVRDisplayComponent` reports `IsDisplayOnDesktop() = true` and
   `IsDisplayRealDisplay() = false`, and returns an identity distortion: the
   host does the bending, so the compositor must not do it twice.
5. When `vrcompositor` swaps a buffer, the daemon's GL renderer takes the first
   layer of the XR window and runs `awl_xr_draw()` instead of the normal quad:
   per-eye barrel pre-distortion, then a rotation (`q_now ⊗ conj(q_at_frame)`)
   applied in tangent space — that is the timewarp that keeps a 60 fps image
   following a 250 Hz pose.
6. Controllers: the driver adds two `vive_controller`-profile devices whose
   pose is the head pose plus a fixed shoulder offset. They are not
   independently tracked — there is no SLAM to track them with.

## Verifying a session

- `anland-steamvr status` — a display block plus `pose: seq=… quat=(…)` means
  the whole chain is alive. `display: <missing>` means the daemon is not running
  with `xr_enabled=1` (or never restarted after the edit).
- `/data/local/tmp/awl_daemon.log` — one `xr: window <id> …` line per XR window,
  and `xr: hiding …` style chatter from the layer goes to SteamVR's own log.
- `logcat -s AwlWindowActivity` on the host — `applyXrState` logs the geometry
  it was handed.
- If SteamVR starts but the HMD tab shows "no headset", check the driver
  directory name: SteamVR looks for `drivers/<name>/bin/<platform><arch>`, and
  arm64 Linux uses `linuxarm64`.

## Known limits

- **3DoF only.** Positional tracking needs SLAM; the phone/glasses give an
  orientation, so `vecPosition` stays at the origin. Seated/standing 360°
  content works, room-scale does not.
- **One XR window.** The first matching window becomes the headset view; any
  further XR windows would just be more windows.
- **Controllers are a gamepad in disguise.** Two devices, fixed offset, no
  individual orientation. Buttons map to the standard OpenVR profile so SteamVR
  input bindings work.
- **No direct mode.** By design: the compositor stays windowed and the host
  composites. Anything that hard-requires `IVRDriverDirectModeComponent`
  (some legacy titles) will not find it.
- **The window is the render target.** `vrcompositor`'s swapchain size is
  whatever SteamVR picks for the window; `xr_eye_w/h` only describes the eye
  images the daemon samples from it. Mismatched aspect ratios show up as
  stretching, not as a resolution change.
- **Latency is host latency.** Pose → daemon → shader is one frame plus the
  Android sensor pipeline; `xr_refresh` cannot fix that, timewarp only hides it
  between poses.
- **Prototype.** No automated tests for the XR path; it was written against
  SteamVR 2.15.6's `openvr_driver.h` and validated by compilation
  (`x86_64 -fsyntax-only`) plus review, not on a device.
