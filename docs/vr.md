# VR: running SteamVR in an anland window

> 中文版：[vr.zh.md](vr.zh.md) · English (this file)

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
│ (or 6DoF     │  input   │  • reprojection       │           │               │
│  provider)   │  pose    │  • anland-xr-display  │           └───────────────┘
└──────────────┘          │  • anland-xr-pose     │ ────────►  driver_anland.so
                          └───────────────────────┘  seqlock   (HMD + 2 pads)
```

## What is new

| Piece | Where | Job |
| --- | --- | --- |
| `AwlXrSession` | `app/libawl` | 3DoF head pose from the IMU (plus angular velocity), gamepad axes/buttons, pushed over binder at ~250 Hz / 60 Hz |
| `awl_xr.cpp` | `services/waylandbridge` | marks XR windows, runs the distortion + reprojection pass, publishes `anland-xr-*` files, adopts the optional 6DoF provider |
| `anland_xr_wire.h` | `xr/include` | the shared file layout (display config, pose block, input block) |
| `driver_anland.so` | `xr/driver` | OpenVR server driver: HMD (`IVRDisplayComponent`) + two controllers |
| `libVkLayer_anland_vr.so` | `xr/layer` | implicit Vulkan layer that hides `VK_KHR_display` while an XR session is on |
| `anland-steamvr` | `xr/` | install / status / start / stop helper |
| `xr_wire_test` | `xr/tools` | self test for the display/pose/input wire format (`xr/build.sh --test`, also run by `xr/build.sh` and CI) |

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

## Root, sensors and what it actually buys you

anland already runs its daemon as root — that is the premise of the whole
design, not an extra step VR asks for. So this tree takes the root path by
default (`xr_imu = 1`): `waylandbridge` opens the gyroscope and accelerometer
itself and integrates them, instead of leaning on the orientation Android fuses
for a normal app.

What that changes:

- **Sampling.** A 2D activity usually gets `SENSOR_DELAY_GAME` fused output at
  ~50 Hz after system fusion; the raw IIO device runs at 200–1000 Hz, which is
  what makes prediction meaningful. `xr_imu_hz` sets the reader's rate (the
  device's own `sampling_frequency` is raised to match when it can be).
- **Access.** A headset may hand a plain window nothing at all (no game rotation
  vector, or a frozen one). Reading the device directly sidesteps that policy
  question entirely.
- **One less hop.** The pose goes daemon → seqlock → guest driver, without the
  binder round trip through the activity.

What it does **not** change:

- **Still 3DoF.** No amount of root turns a gyro into a position. Real 6DoF needs
  a pose source — camera VIO, the headset's own tracking, or a runtime bridge —
  and the hand-off for that is `anland-xr-pose-ext` (below). Root makes it
  *possible* to run a camera + IMU tracker in the daemon later; this tree does
  not ship one.
- **No direct mode.** Presenting straight to the panel needs
  `IVRDriverDirectModeComponent` and a swapchain the compositor can scan out,
  which contradicts "SurfaceFlinger still composes everything". That stays a
  deliberate non-goal, root or not.

The daemon logs which device it picked — `logcat -s anland-imu`:

```
imu: reading iio:device1 (gyro) and iio:device1 (accel) at 200 Hz
```

If auto-detection picks the wrong one, set `xr_imu_gyro` / `xr_imu_accel` to
a concrete path — `ls /sys/bus/iio/devices/*/name` tells you what the kernel
calls them. `xr_imu = 0` falls back to the activity's fused pose; the external
6DoF file, when present and fresh, outranks both.

## Turning it on

Everything here is also on the phone: **Window Settings → VR**. The settings
page reads the live values from the daemon over binder (`CFG_GET`/`CFG_SET`, plus
the string transaction for the title match list), writes straight back into
`config.json`, and applies immediately — the VR window gets a fresh `C_XR` and a
repaint, so distortion tuning happens through the lenses while SteamVR runs.
The Quest 3S preset button fills in the geometry numbers below.

The config file still works, for scripting and for the first boot:
`/data/adb/modules/anland-awl/config.json` (edit, then `stop && start` the
module service or reboot):

```json
{
  "sc_enabled": 0,
  "xr_enabled": 1,
  "xr_mode": 2,
  "xr_fov": 90,
  "xr_ipd": 63,
  "xr_k1": 0,
  "xr_k2": 0,
  "xr_predict": 20
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
| `xr_eye_w`, `xr_eye_h` | `0` | 0–4096 | per-eye render size; `0` = follow the XR window's surface |
| `xr_fov` | `90` | 30–170 | horizontal FOV per eye in degrees; the vertical one follows the eye aspect |
| `xr_ipd` | `63` | 20–90 | IPD in millimetres |
| `xr_scale` | `1000` | 200–2000 | per-mille multiplier on the per-eye size |
| `xr_k1`, `xr_k2` | `0` | −1000–2000 | per-mille barrel distortion coefficients; `0` = no lens correction, straight pass-through |
| `xr_ca` | `0` | 0–100 | per-mille chromatic aberration compensation |
| `xr_predict` | `0` | 0–120 | milliseconds of head motion extrapolated past the newest pose sample; `0` = no prediction |
| `xr_imu` | `0` | 0–1 | read the raw gyroscope/accelerometer as root instead of the fused pose Android gives a 2D activity |
| `xr_imu_hz` | `200` | 50–1000 | sensor sampling rate for that reader |
| `xr_imu_gyro`, `xr_imu_accel` | `""` | — | device to read; empty = auto-detect. Accepts a sysfs path, a `/dev` path, or part of the device name |
| `xr_match` | `vrcompositor,steamvr` | — | comma-separated, case-insensitive substrings that mark a window as XR |

`xr_k1`/`xr_k2` are for cardboard-style lenses; for AR glasses, or a phone
panel looked at directly, leave them at `0` and the pass becomes a plain
(then reprojected) blit.

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
  "xr_ca": 10,
  "xr_predict": 20,
  "xr_imu": 1,
  "xr_imu_hz": 200
}
```

- `xr_eye_w/h = 0` follows the window: the daemon adopts whatever surface size
  the host hands the XR window and re-publishes the display block, so the eye
  target cannot drift away from the compositor's swapchain. If the resulting
  aspect ratio contradicts `xr_mode`, the daemon logs one warning instead of
  silently stretching.
- `xr_scale = 900` renders slightly under native and lets the reprojection
  stretch do the rest — the first knob to turn if `vrcompositor` reports late
  frames.
- The `k1`/`k2` above are a starting point for the Q3S lens, not a measurement:
  look at a straight grid (SteamVR's room outline or `steamvr.vrsettings`'
  `driver_anland` test pattern) and raise `k1` until the edges stop bowing
  outward. `xr_ca` trims the colour fringing that shows up at the rim.
- `xr_predict = 20` extrapolates the pose 20 ms using the IMU's angular
  velocity. Raise it while turning your head: too little and the world drags
  behind, too much and it overshoots. 0 turns prediction off.
- `xr_imu = 1` is what makes that angular velocity worth anything: a 2D window
  on Horizon OS can be given no orientation at all, and the fused pose it does
  get is capped at ~50 Hz. The daemon reads the headset's own IIO devices
  instead, at 200 Hz by default — see [Root, sensors and what it actually buys
  you](#root-sensors-and-what-it-actually-buys-you).
- If `./anland-steamvr status` shows a `seq` that keeps counting while the
  quaternion stays at `0,0,0,1`, check that `xr_imu` is on and that
  `logcat -s anland-imu` found a device. If the headset still yields nothing,
  set `xr_mode = 1` and use SteamVR's seated mode, or feed poses from an OpenXR
  bridge through `anland-xr-pose-ext` (below). Everything else (SBS output,
  distortion, controllers) works either way.
- Root is a hard requirement in any case: `anland` is a KernelSU/SukiSU module
  plus a binder daemon in the `awl_daemon` domain.

Then, inside the container:

```sh
tar xzf anlandx.tar.gz && bash anlandx/setupanlandx.sh   # once, as usual
cd anlandx/xr
./build.sh                    # wire self test, driver + layer into xr/build/
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
3. Every IMU sample goes back as `AWL_T_XR_POSE2` (quaternion, position,
   angular velocity), gamepad state as `AWL_T_XR_INPUT`. The daemon keeps the
   latest pose in a seqlock and mirrors it to
   `$ANLAND_RUNTIME_DIR/anland-xr-pose`, inputs to `anland-xr-input`.
4. The driver's `RunFrame()` reads those files and feeds OpenVR. Its
   `IVRDisplayComponent` reports `IsDisplayOnDesktop() = true` and
   `IsDisplayRealDisplay() = false`, and returns an identity distortion: the
   host does the bending, so the compositor must not do it twice.
5. When `vrcompositor` swaps a buffer, the daemon's GL renderer takes the first
   layer of the XR window and runs `awl_xr_draw()` instead of the normal quad:
   per-eye barrel pre-distortion, then a rotation
   (`q_now ⊗ conj(q_at_frame)`) applied in tangent space — that is the
   reprojection that keeps a 60 fps image following a 250 Hz pose. `q_now` is
   the newest pose optionally extrapolated by `xr_predict`; `q_at_frame` is the
   pose that window's previous frame was drawn with, so several XR windows each
   reproject against their own reference.
6. Controllers: the driver adds two `vive_controller`-profile devices whose
   pose is the head pose plus a fixed shoulder offset, unless the input block
   carries a real pose for them (see below).

## External 6DoF provider

The IMU session is 3DoF because a phone has no SLAM. A process that *does* have
tracking — an ARCore activity on a phone, an OpenXR bridge on a headset, a
6DoF tracker daemon — can take over the pose without touching anland's code by
publishing to `$ANLAND_RUNTIME_DIR/anland-xr-pose-ext`:

```c
struct anland_xr_pose_block* blk = /* mmap 4096 B of ANLAND_XR_POSE_EXT_FILE */;

anland_xr_seq_begin(&blk->seq);
blk->magic        = ANLAND_XR_POSE_MAGIC;
blk->pose.sample_ns = now_ns;          /* CLOCK_MONOTONIC, strictly increasing */
blk->pose.quat[4]   = q;               /* x, y, z, w — +x right, +y up, -z forward */
blk->pose.pos[3]    = p;               /* metres; room-scale position */
blk->pose.ang_vel[3]= w;               /* rad/s, optional (0 = derive) */
blk->pose.tracking  = 1;               /* 0 = provider has no fix right now */
anland_xr_seq_end(&blk->seq);
```

Rules the daemon applies:

- A pose only wins when `sample_ns` is newer than the pose it already has, so
  publishing the same sample twice is harmless.
- While the file keeps updating (within 250 ms), poses from the root IMU reader
  and from `AwlXrSession` are both dropped — sources at different rates would
  fight and the reprojection would jitter.
- `tracking = 0`, a missing file, or 250 ms of silence hands the pose back to
  whatever was underneath: the root IMU reader if `xr_imu = 1`, else the
  activity's fused pose. Nothing needs to be torn down; the next sample from
  that source just wins again.
- The file is read `O_RDONLY` and never created by the daemon — the provider
  owns its lifetime. Delete it to switch back to 3DoF for good.

The same trick works one level down: if something writes real per-controller
poses into `anland-xr-input` (`c[i].quat`/`c[i].pos` non-identity), the driver
uses them instead of the head-plus-shoulder fallback.

## Verifying a session

- `anland-steamvr status` — a display block plus `pose: seq=… quat=(…)` means
  the whole chain is alive. `display: <missing>` means the daemon is not running
  with `xr_enabled=1` (or never restarted after the edit).
- `/data/local/tmp/awl_daemon.log` — one `xr: window <id> …` line per XR window,
  `xr: render target follows the window: …` when a surface size arrives,
  `xr: root IMU on (…)` when the root reader started, `xr: root IMU provider is
  live - Activity poses are paused` on the first accepted sample, and
  `xr: external pose provider is live …` when `anland-xr-pose-ext` takes over.
- `logcat -s anland-imu` — which IIO device the daemon picked and at what rate.
- `logcat -s AwlWindowActivity` on the host — `applyXrState` logs the geometry
  it was handed, and `asked for <n> Hz` whether the panel accepted the mode.
- `cd anlandx/xr && ./build.sh --test` — checks the display/pose/input wire
  format on any Linux box, no phone involved.
- If SteamVR starts but the HMD tab shows "no headset", check the driver
  directory name: SteamVR looks for `drivers/<name>/bin/<platform><arch>`, and
  arm64 Linux uses `linuxarm64`.

## Known limits

Each of the original limits, and where it stands now:

| Limit | Status |
| --- | --- |
| **3DoF only** | **Narrowed.** Tracking now reads the raw gyro/accelerometer as root (`xr_imu`) at up to 1 kHz, so neither sensor throttling nor the headset's policy on 2D windows applies. It is still orientation-only: real 6DoF needs a pose source, and the hand-off for one is `anland-xr-pose-ext`. |
| **High pose latency** | **Narrowed.** `xr_predict` (0–120 ms) extrapolates with angular velocity both in the driver's `vecAngularVelocity` and in the reprojection, and the root reader removes the activity→binder hop from the pose path. The host composite still costs a frame. |
| **One XR window** | **Narrowed.** The reprojection reference is per window now, so a second XR window reprojects against its own previous frame instead of double-rotating. All XR windows still share one head pose, and only one can sensibly be the headset view. |
| **The window is the render target** | **Narrowed.** With `xr_eye_w/h = 0` the eye target follows the XR window's surface size and is re-published on every resize; a mismatched aspect ratio logs one warning instead of silently stretching. SteamVR still decides the swapchain size. |
| **Controllers are a gamepad** | **Partly.** The wire format and driver accept real per-controller poses; stock Android has no tracker that produces them. Buttons/axes map to the `vive_controller` profile either way. |
| **No direct mode** | **By design, not solvable here.** Direct mode needs `IVRDriverDirectModeComponent`, a Vulkan present interception and a virtual display, i.e. exactly the design anland rejected: SurfaceFlinger stays the compositor. Titles that hard-require it will not find it. |
| **Prototype** | **Narrowed.** `xr/tools/xr_wire_test.c` round-trips the display file and both seqlock blocks, runs in `xr/build.sh` and in CI. Device behaviour (distortion numbers, SteamVR interplay) is still untested on hardware. |

What the XR path deliberately still does not do:

- **Take the display over.** Direct mode, hardware scanout and per-eye
  `VK_KHR_display` planes are the opposite of anland's model.
- **Track controllers.** There is no camera or lighthouse input; a tracker
  bridge can feed poses in, but nothing in anland produces them.
- **Predict the future properly.** `xr_predict` is a constant-velocity
  extrapolation on the newest sample, not a filter with acceleration, and it is
  fed by whatever rate the IMU or the provider runs at.
