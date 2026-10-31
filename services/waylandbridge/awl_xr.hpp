/* awl_xr.hpp — XR ("VR headset") compositing for anland windows.
 *
 * Model: a window whose title matches xr_match (the SteamVR compositor is one)
 * is composited through a per-eye lens-distortion + timewarp pass instead of
 * the plain quad the GL renderer would draw. The window itself is an ordinary
 * Wayland toplevel — the guest keeps presenting into it, so nothing about the
 * frame path changes (dmabuf → forged AHardwareBuffer → EGLImage, zero copy);
 * only the last sampling step is replaced.
 *
 * The host owns the head pose: the attached Activity runs an IMU session and
 * pushes poses over binder (AWL_T_XR_POSE). We republish them into a seqlock
 * file that the container-side OpenVR driver reads, and we use them here for
 * asynchronous reprojection between guest frames.
 *
 * XR needs the GL backend (sc_enabled=0): with SurfaceControl the layers go
 * straight to SurfaceFlinger and there is no place to distort them.
 */
#ifndef AWL_XR_HPP
#define AWL_XR_HPP

#include <cstddef>
#include <cstdint>

/* Mirrors of the daemon config keys (config.json). See docs/vr.md. */
struct awl_xr_config {
    bool enabled = false;      /* xr_enabled */
    int mode = 2;              /* xr_mode: 1 mono, 2 sbs-h, 3 sbs-v */
    int refresh = 0;           /* xr_refresh: Hz, 0 = panel refresh */
    int eye_w = 0;             /* xr_eye_w: 0 = derived from the panel */
    int eye_h = 0;             /* xr_eye_h */
    float fov_deg = 90.0f;     /* xr_fov: horizontal FOV per eye, degrees */
    float ipd_mm = 63.5f;      /* xr_ipd */
    float k1 = 0.0f;           /* xr_k1 / xr_k2: barrel pre-distortion */
    float k2 = 0.0f;
    float render_scale = 1.0f; /* xr_render_scale: eye target scale */
    float ca = 0.0f;           /* xr_ca: chromatic aberration strength */
    int predict_ms = 0;        /* xr_predict: extrapolate the pose by this much */
    char match[256] = "vrcompositor,steamvr"; /* xr_match: comma separated titles */
};

/* Boot the subsystem: mmap the pose/input files, publish anland-xr-display.
 * panel_w/panel_h/refresh_hz describe the Android display the window is
 * attached to (used to derive the eye target when the config leaves it 0).
 * Safe to call once; idempotent afterwards. */
bool awl_xr_start(const awl_xr_config& cfg, uint32_t panel_w, uint32_t panel_h,
                  double refresh_hz);

/* Live re-config (config set xr_*): republishes the display file and drops the
 * per-window marks so the next title change re-evaluates them. */
void awl_xr_apply(const awl_xr_config& cfg);

const awl_xr_config& awl_xr_get(void);
bool awl_xr_enabled(void);

/* Effective geometry (derived from the config + the panel when the config
 * leaves it 0). Used for the C_XR control message. */
void awl_xr_geometry(uint32_t* eye_w, uint32_t* eye_h, uint32_t* mode, double* refresh_hz);

/* Comma-separated, case-insensitive substring match against a window title. */
bool awl_xr_title_matches(const char* title);

/* Mark/unmark a wayland window as the XR window. Idempotent. */
void awl_xr_mark_window(uint64_t id, bool xr);
bool awl_xr_window_active(uint64_t id);

/* Latest pose, for the renderer's stale check. */
bool awl_xr_pose_valid(void);

/* Head pose from the Android XR session (binder T_XR_POSE / T_XR_POSE2).
 * Quaternion is x,y,z,w in the driver tracking space; sample_ns is
 * CLOCK_MONOTONIC; ang_vel is rad/s in the same frame (may be NULL — then the
 * daemon estimates it by differencing quaternions, which is what xr_predict
 * extrapolates with). */
void awl_xr_set_pose(const float quat[4], const float pos[3], uint64_t sample_ns,
                     uint32_t period_ns);
void awl_xr_set_pose_vel(const float quat[4], const float pos[3],
                         const float ang_vel[3], uint64_t sample_ns, uint32_t period_ns);

/* Controller/input snapshot (binder T_XR_INPUT): 2 controllers, the same
 * field order the Activity writes. Republished as anland-xr-input. */
void awl_xr_set_input(const int32_t* ints, size_t nints);

/* Tell the pass how big the XR window's surface currently is. The eye target
 * follows it when the config leaves xr_eye_w/h at 0, and a mismatch with the
 * declared mode (e.g. 2:1 SBS content in a 1:1 window) is logged once: that is
 * the "the window is the render target" trap. Call on attach and on resize. */
void awl_xr_note_window_size(uint64_t id, uint32_t w, uint32_t h);

/* Record the pose used by the frame that was just drawn. Called by the
 * renderer right after awl_xr_draw(). Per window: two XR windows may show
 * frames composed at different instants. */
void awl_xr_note_render(uint64_t id);

/* Composite one window frame through the XR pass. tex is the layer texture
 * (owned by the renderer), (u0,v0,su,sv) its sampling sub-rect. Must run on the
 * render thread with its EGL context current. */
void awl_xr_draw(uint64_t id, uint32_t tex, uint32_t bw, uint32_t bh, int vw, int vh,
                 float u0, float v0, float su, float sv);

void awl_xr_shutdown(void);

#endif /* AWL_XR_HPP */
