/*
 * anland XR wire format.
 *
 * One header, two sides:
 *   host  = waylandbridge (Android, root daemon). It owns the head pose (the
 *           phone's IMU), decides the virtual display geometry and writes the
 *           files below into the daemon runtime dir (ANLAND_RUNTIME_DIR,
 *           /data/local/tmp/awl on the Android side, bind-mounted to /run/anland
 *           inside the container).
 *   guest = the OpenVR driver shipped in xr/driver (container side). It reads
 *           the files, publishes SteamVR poses and reports the display
 *           geometry back to vrserver.
 *
 * Everything here is layout-only: no libc allocation, no Android headers, so
 * the same bytes are shared by a bionic C++17 daemon, an aarch64 glibc driver
 * and (in principle) anything else that speaks the format.
 *
 * Files (all in the runtime dir):
 *   anland-xr-display   key=value text, host → guest. Written once at startup
 *                       and on every xr_* config change; small, line oriented,
 *                       so a user can cat/edit it while debugging.
 *   anland-xr-pose      anland_xr_pose_block, host → guest, seqlock, 4096 B.
 *   anland-xr-input     anland_xr_input_block, host → guest, seqlock, 4096 B.
 *
 * Seqlock contract: the writer bumps `seq` to an odd value, writes the body,
 * then bumps it to an even value again. A reader samples `seq`, copies the
 * body, re-samples `seq`: equal and even means the copy is consistent. The
 * files are mmapped by both sides, so a torn read is possible by design — the
 * seqlock is what makes it detectable. Never read the body without checking.
 */
#ifndef ANLAND_XR_WIRE_H
#define ANLAND_XR_WIRE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANLAND_XR_WIRE_VERSION 1

/* One block per file; fixed so both sides can mmap a private copy of the same
 * size regardless of struct growth in later versions. */
#define ANLAND_XR_BLOCK_BYTES 4096

#define ANLAND_XR_DISPLAY_FILE "anland-xr-display"
#define ANLAND_XR_POSE_FILE "anland-xr-pose"
#define ANLAND_XR_INPUT_FILE "anland-xr-input"

#define ANLAND_XR_POSE_MAGIC 0x41585031u  /* "AXP1" */
#define ANLAND_XR_INPUT_MAGIC 0x41584931u /* "AXI1" */

/* xr_mode: how the guest compositor lays its output out inside the window the
 * host composites. 0 = XR off (plain window), 1 = one mono image, 2 = stereo
 * side by side horizontally, 3 = stereo side by side vertically. */
#define ANLAND_XR_MODE_OFF 0
#define ANLAND_XR_MODE_MONO 1
#define ANLAND_XR_MODE_SBS_H 2
#define ANLAND_XR_MODE_SBS_V 3

#define ANLAND_XR_MAX_CONTROLLERS 2

/* Controller buttons. Left and right controllers share the bit layout: c[0]
 * carries X/Y where c[1] carries A/B (the face button pair of that hand). */
#define ANLAND_XR_BTN_A (1u << 0)
#define ANLAND_XR_BTN_B (1u << 1)
#define ANLAND_XR_BTN_MENU (1u << 2)
#define ANLAND_XR_BTN_SYSTEM (1u << 3)
#define ANLAND_XR_BTN_STICK_CLICK (1u << 4)
#define ANLAND_XR_BTN_GRIP_CLICK (1u << 5)
#define ANLAND_XR_BTN_TRIGGER_CLICK (1u << 6)
#define ANLAND_XR_BTN_DPAD_UP (1u << 7)
#define ANLAND_XR_BTN_DPAD_DOWN (1u << 8)
#define ANLAND_XR_BTN_DPAD_LEFT (1u << 9)
#define ANLAND_XR_BTN_DPAD_RIGHT (1u << 10)
#define ANLAND_XR_BTN_THUMBREST (1u << 11)

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* runtime dir                                                         */
/* ------------------------------------------------------------------ */

/* The container side exports ANLAND_RUNTIME_DIR (anlandx does; see
 * anland-session/anland-session.sh). The Android daemon always passes the
 * path explicitly instead of relying on the environment. */
static inline const char* anland_xr_runtime_dir(void) {
    const char* r = getenv("ANLAND_RUNTIME_DIR");
    return (r && r[0]) ? r : "/run/anland";
}

/* dir/name → out; 0 on success, -1 when it would not fit */
static inline int anland_xr_path(const char* name, char* out, size_t n) {
    int w = snprintf(out, n, "%s/%s", anland_xr_runtime_dir(), name);
    return (w > 0 && (size_t)w < n) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* anland-xr-display                                                   */
/* ------------------------------------------------------------------ */

struct anland_xr_display {
    uint32_t version;
    uint32_t mode; /* ANLAND_XR_MODE_* */
    uint32_t eye_width;
    uint32_t eye_height;
    double refresh_hz;
    /* Half-angle tangents of each eye's frustum, in the guest's eye space
     * (+x right, +y up, looking down -z). */
    double fov_left, fov_right, fov_top, fov_bottom;
    double ipd;          /* metres, interpupillary distance */
    double render_scale; /* 1.0 = panel resolution */
};

static inline void anland_xr_display_defaults(struct anland_xr_display* d) {
    memset(d, 0, sizeof(*d));
    d->version = ANLAND_XR_WIRE_VERSION;
    d->mode = ANLAND_XR_MODE_SBS_H;
    d->eye_width = 1024;
    d->eye_height = 1024;
    d->refresh_hz = 72.0;
    d->fov_left = -1.0;
    d->fov_right = 1.0;
    d->fov_top = 1.0;
    d->fov_bottom = -1.0;
    d->ipd = 0.0635;
    d->render_scale = 1.0;
}

static inline void anland_xr_display_write(FILE* f, const struct anland_xr_display* d) {
    fprintf(f,
            "# anland XR virtual display — written by waylandbridge, read by the\n"
            "# anland OpenVR driver. Values are recomputed on daemon start and on\n"
            "# every xr_* config change.\n"
            "version=%u\n"
            "mode=%u\n"
            "eye_width=%u\n"
            "eye_height=%u\n"
            "refresh_hz=%.3f\n"
            "fov_left=%.6f\n"
            "fov_right=%.6f\n"
            "fov_top=%.6f\n"
            "fov_bottom=%.6f\n"
            "ipd=%.6f\n"
            "render_scale=%.4f\n",
            d->version, d->mode, d->eye_width, d->eye_height, d->refresh_hz,
            d->fov_left, d->fov_right, d->fov_top, d->fov_bottom, d->ipd,
            d->render_scale);
}

/* Returns 1 when at least version/eye size were readable, 0 otherwise (the
 * driver then keeps its defaults). Missing keys keep the incoming value, so a
 * caller seeds the struct with anland_xr_display_defaults() first. */
static inline int anland_xr_display_parse(const char* text, struct anland_xr_display* d) {
    int seen = 0;
    const char* p = text;
    while (p && *p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        char* eq = strchr(line, '=');
        if (eq && line[0] != '#') {
            *eq = 0;
            const char* k = line;
            const char* v = eq + 1;
            if (!strcmp(k, "version")) { d->version = (uint32_t)strtoul(v, NULL, 10); seen++; }
            else if (!strcmp(k, "mode")) d->mode = (uint32_t)strtoul(v, NULL, 10);
            else if (!strcmp(k, "eye_width")) { d->eye_width = (uint32_t)strtoul(v, NULL, 10); seen++; }
            else if (!strcmp(k, "eye_height")) { d->eye_height = (uint32_t)strtoul(v, NULL, 10); seen++; }
            else if (!strcmp(k, "refresh_hz")) d->refresh_hz = strtod(v, NULL);
            else if (!strcmp(k, "fov_left")) d->fov_left = strtod(v, NULL);
            else if (!strcmp(k, "fov_right")) d->fov_right = strtod(v, NULL);
            else if (!strcmp(k, "fov_top")) d->fov_top = strtod(v, NULL);
            else if (!strcmp(k, "fov_bottom")) d->fov_bottom = strtod(v, NULL);
            else if (!strcmp(k, "ipd")) d->ipd = strtod(v, NULL);
            else if (!strcmp(k, "render_scale")) d->render_scale = strtod(v, NULL);
        }
        p = eol ? eol + 1 : NULL;
    }
    if (d->eye_width == 0 || d->eye_height == 0) return 0;
    if (d->fov_right <= d->fov_left || d->fov_top <= d->fov_bottom) return 0;
    return seen ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* anland-xr-pose                                                      */
/* ------------------------------------------------------------------ */

/* Head pose in the driver's tracking space (+x right, +y up, -z forward),
 * metres and a unit quaternion. The host samples the phone IMU; quat is the
 * orientation of the head in that space, pos its translation (3DoF sessions
 * keep pos at zero and only yaw/pitch/roll change). */
struct anland_xr_pose {
    uint64_t sample_ns;   /* CLOCK_MONOTONIC ns at sensor sample */
    uint64_t period_ns;   /* sensor period, 0 = unknown */
    double quat[4];       /* x, y, z, w */
    double pos[3];        /* metres */
    double ang_vel[3];    /* rad/s (optional, 0 = unknown) */
    double ipd;           /* metres, 0 = use the display value */
    uint32_t tracking;    /* 1 = 3DoF valid, 0 = no head tracking */
    uint32_t reserved;
};

struct anland_xr_pose_block {
    uint32_t seq;
    uint32_t magic;
    struct anland_xr_pose pose;
    uint8_t pad[ANLAND_XR_BLOCK_BYTES - 2 * sizeof(uint32_t) - sizeof(struct anland_xr_pose)];
};

/* ------------------------------------------------------------------ */
/* anland-xr-input                                                     */
/* ------------------------------------------------------------------ */

struct anland_xr_controller {
    uint32_t present;
    uint32_t buttons;   /* ANLAND_XR_BTN_* */
    float trigger;      /* 0..1 */
    float grip;         /* 0..1 */
    float stick_x;      /* -1..1 */
    float stick_y;
    float trackpad_x;   /* -1..1, 0 when the pad reads the stick */
    float trackpad_y;
    float pad_pressed;  /* 0..1, drives /input/trackpad/press */
    double quat[4];     /* 3DoF orientation (identity until the host tracks hands) */
    double pos[3];
};

struct anland_xr_input {
    uint64_t sample_ns;
    uint32_t n;
    uint32_t reserved;
    struct anland_xr_controller c[ANLAND_XR_MAX_CONTROLLERS];
};

struct anland_xr_input_block {
    uint32_t seq;
    uint32_t magic;
    struct anland_xr_input input;
    uint8_t pad[ANLAND_XR_BLOCK_BYTES - 2 * sizeof(uint32_t) - sizeof(struct anland_xr_input)];
};

/* ------------------------------------------------------------------ */
/* seqlock helpers (the same inline code on both sides)                */
/* ------------------------------------------------------------------ */

static inline uint32_t anland_xr_seq_read(const volatile uint32_t* seq) {
    return __atomic_load_n(seq, __ATOMIC_ACQUIRE);
}

static inline void anland_xr_seq_begin(volatile uint32_t* seq) {
    __atomic_store_n(seq, anland_xr_seq_read(seq) + 1, __ATOMIC_RELEASE);
}

static inline void anland_xr_seq_end(volatile uint32_t* seq) {
    __atomic_store_n(seq, anland_xr_seq_read(seq) + 1, __ATOMIC_RELEASE);
}

/* True when both samples are equal and the counter is even (a completed
 * write). Callers keep the two samples around the body copy. */
static inline int anland_xr_seq_stable(uint32_t before, uint32_t after) {
    return before == after && (before & 1u) == 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ANLAND_XR_WIRE_H */
