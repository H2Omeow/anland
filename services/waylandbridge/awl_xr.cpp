/* awl_xr.cpp — XR compositing pass for waylandbridge.
 *
 * See awl_xr.hpp for the model. This file owns:
 *   - the daemon side of the XR wire format (anland_xr_wire.h): it mmaps
 *     anland-xr-pose / anland-xr-input and republishes the head pose and the
 *     controller snapshot the Activity pushes over binder, and it writes
 *     anland-xr-display so the guest driver reports a matching frustum;
 *   - the GL pass that turns one window texture into two distorted eyes with
 *     asynchronous reprojection.
 *
 * The pass runs on the render thread with that window's EGL context current
 * (awl_renderer's render_frame calls it between buffer import and swap).
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <GLES3/gl3.h>

#include <map>
#include <mutex>

#define AWL_TAG "anland-xr"
#include "awl_log.h"

#include "anland_xr_wire.h"
#include "awl_xr.hpp"

namespace {

/* Pose older than this is not used for reprojection (the sensor session died,
 * the Activity went away, or the user paused): the last frame is shown as-is
 * rather than warped by a stale rotation. */
constexpr uint64_t kPoseFreshNs = 250ull * 1000ull * 1000ull;

struct state {
    std::mutex lock;
    awl_xr_config cfg;
    std::map<uint64_t, bool> windows; /* id → is the XR window */

    /* latest head pose (driver tracking space) */
    double q[4] = {0.0, 0.0, 0.0, 1.0};
    double pos[3] = {0.0, 0.0, 0.0};
    double ang_vel[3] = {0.0, 0.0, 0.0};
    uint64_t pose_ns = 0;
    uint32_t period_ns = 0;
    bool pose_ok = false;

    /* last pose adopted from the optional 6DoF provider
     * (anland-xr-pose-ext). While that file keeps updating, IMU poses from the
     * Activity are dropped: mixing two 60/250 Hz sources into one quaternion
     * would make the reprojection jitter instead of track. */
    uint64_t ext_ns = 0;
    bool ext_logged = false;

    /* last pose produced by the daemon's own root-side IMU reader. Also a
     * "provider is live" marker: while it keeps updating, the Activity's IMU
     * poses are ignored, so the two never interleave. */
    uint64_t imu_ns = 0;
    bool imu_logged = false;

    /* pose the previous XR frame of a given window was drawn with. Per window:
     * two XR windows can be showing frames composed at different instants, and
     * reprojecting one with the other's reference would double-rotate it. */
    struct window_ref {
        double q[4] = {0.0, 0.0, 0.0, 1.0};
    };
    std::map<uint64_t, window_ref> refs;

    /* size of the XR window's surface; the eye target follows it when the
     * config leaves xr_eye_w/h at 0 */
    uint32_t win_w = 0, win_h = 0;
    bool aspect_warned = false;

    /* last input snapshot as parsed from binder */
    anland_xr_input input;

    /* published display geometry */
    anland_xr_display disp;
    uint32_t panel_w = 0, panel_h = 0;
    double panel_hz = 72.0;
    bool started = false;
};

state g;

int g_pose_fd = -1;
int g_input_fd = -1;
int g_ext_fd = -1; /* optional 6DoF provider: anland-xr-pose-ext */
anland_xr_pose_block* g_pose_map = nullptr;
anland_xr_input_block* g_input_map = nullptr;
anland_xr_pose_block* g_ext_map = nullptr;

GLuint g_prog = 0;
GLuint g_vao = 0;
GLint g_u_tex = -1, g_u_src = -1, g_u_fov = -1, g_u_k = -1, g_u_warp = -1,
      g_u_ca = -1, g_u_mode = -1;
bool g_gl_failed = false;

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool ci_contains(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    const size_t nl = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return true;
        if (!p[i]) break;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* mmapped seqlock blocks                                              */
/* ------------------------------------------------------------------ */

void* map_block(const char* name, int* fd_out) {
    char path[512];
    anland_xr_path(name, path, sizeof(path));
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) {
        LOGE("xr: open %s: %s", path, strerror(errno));
        return nullptr;
    }
    if (ftruncate(fd, ANLAND_XR_BLOCK_BYTES) != 0) {
        LOGE("xr: ftruncate %s: %s", path, strerror(errno));
        close(fd);
        return nullptr;
    }
    void* p = mmap(nullptr, ANLAND_XR_BLOCK_BYTES, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        LOGE("xr: mmap %s: %s", path, strerror(errno));
        close(fd);
        return nullptr;
    }
    chmod(path, 0666); /* the container user is not the daemon's uid */
    *fd_out = fd;
    LOGI("xr: %s mapped", path);
    return p;
}

/* Read-only mapping for a block somebody else owns (the optional 6DoF
 * provider). Absent file is not an error: 3DoF is the default. */
void* map_block_ro(const char* name, int* fd_out) {
    char path[512];
    anland_xr_path(name, path, sizeof(path));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    void* p = mmap(nullptr, ANLAND_XR_BLOCK_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    *fd_out = fd;
    LOGI("xr: %s mapped (external pose provider)", path);
    return p;
}

/* Copy a pose block out with the seqlock handshake; false = torn/stale/empty. */
bool read_pose_block(const anland_xr_pose_block* blk, anland_xr_pose* out) {
    if (!blk) return false;
    const volatile uint32_t* seq = (const volatile uint32_t*)&blk->seq;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t before = anland_xr_seq_read(seq);
        if (before & 1u) continue;
        if (blk->magic != ANLAND_XR_POSE_MAGIC) return false;
        memcpy(out, &blk->pose, sizeof(*out));
        uint32_t after = anland_xr_seq_read(seq);
        if (anland_xr_seq_stable(before, after)) return out->tracking != 0;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* quaternion helpers beyond the damped ones below                     */
/* ------------------------------------------------------------------ */

/* q = exp(0.5 * axis_angle): small-angle rotation from a scaled axis. */
void quat_from_scaled_axis(const double r[3], double out[4]) {
    const double angle = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (angle < 1e-9) { out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1; return; }
    const double s = sin(angle * 0.5) / angle;
    out[0] = r[0] * s;
    out[1] = r[1] * s;
    out[2] = r[2] * s;
    out[3] = cos(angle * 0.5);
}

/* Angular velocity (rad/s) between two orientations over dt seconds:
 * dq = conj(q0) ⊗ q1 = exp(0.5 ω dt). */
void quat_to_ang_vel(const double q0[4], const double q1[4], double dt, double out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    if (dt <= 1e-6) return;
    const double c0[4] = {-q0[0], -q0[1], -q0[2], q0[3]};
    const double dq[4] = {
        c0[3] * q1[0] + c0[0] * q1[3] + c0[1] * q1[2] - c0[2] * q1[1],
        c0[3] * q1[1] - c0[0] * q1[2] + c0[1] * q1[3] + c0[2] * q1[0],
        c0[3] * q1[2] + c0[0] * q1[1] - c0[1] * q1[0] + c0[2] * q1[3],
        c0[3] * q1[3] - c0[0] * q1[0] - c0[1] * q1[1] - c0[2] * q1[2],
    };
    double w = dq[3];
    if (w > 1.0) w = 1.0;
    if (w < -1.0) w = -1.0;
    const double angle = 2.0 * acos(w);
    const double sn = sqrt(fmax(0.0, 1.0 - w * w));
    if (sn < 1e-9) return; /* no rotation (or an exact half turn, unhelpful) */
    const double k = angle / (sn * dt);
    out[0] = dq[0] * k;
    out[1] = dq[1] * k;
    out[2] = dq[2] * k;
}


/* ------------------------------------------------------------------ */
/* display geometry (host ↔ guest contract)                            */
/* ------------------------------------------------------------------ */

/* Fill g.disp from the config + panel. Caller holds g.lock. The frustum is
 * the *virtual* display both sides agree on: the guest renders into it, the
 * host maps its panel halves onto it. Square-ish pixels: the vertical tangent
 * follows the eye aspect. */
void compute_display_locked(void) {
    anland_xr_display& d = g.disp;
    anland_xr_display_defaults(&d);
    d.mode = (uint32_t)(g.cfg.mode < 1 ? 1 : g.cfg.mode);
    d.refresh_hz = g.cfg.refresh > 0 ? (double)g.cfg.refresh : g.panel_hz;

    /* What we map onto: the XR window's own surface when we know it (that is
     * the render target), else the panel. Using the window keeps the declared
     * eye pair aspect-identical to what the guest actually composed. */
    const uint32_t src_w = g.win_w ? g.win_w : g.panel_w;
    const uint32_t src_h = g.win_h ? g.win_h : g.panel_h;

    uint32_t ew = (uint32_t)g.cfg.eye_w > 0 ? (uint32_t)g.cfg.eye_w
                                            : (d.mode == ANLAND_XR_MODE_SBS_H ? src_w / 2 : src_w);
    uint32_t eh = (uint32_t)g.cfg.eye_h > 0 ? (uint32_t)g.cfg.eye_h
                                            : (d.mode == ANLAND_XR_MODE_SBS_V ? src_h / 2 : src_h);
    if (ew < 64) ew = 64;
    if (eh < 64) eh = 64;
    const double scale = g.cfg.render_scale > 0.1f ? (double)g.cfg.render_scale : 1.0;
    ew = (uint32_t)fmax(64.0, (double)ew * scale);
    eh = (uint32_t)fmax(64.0, (double)eh * scale);
    /* Keep the published target in a sane band: the eye target is what apps
     * render, and a 4K panel would otherwise ask Adreno for 2x4K per frame. */
    if (ew > 4096) ew = 4096;
    if (eh > 4096) eh = 4096;
    d.eye_width = ew;
    d.eye_height = eh;

    double fov = g.cfg.fov_deg > 10.0f && g.cfg.fov_deg < 170.0f ? g.cfg.fov_deg : 90.0;
    double tan_h = tan(fov * M_PI / 180.0 * 0.5);
    double aspect = (double)ew / (double)eh;
    double tan_v = tan_h / (aspect > 0.01 ? aspect : 1.0);
    d.fov_left = -tan_h;
    d.fov_right = tan_h;
    d.fov_bottom = -tan_v;
    d.fov_top = tan_v;

    d.ipd = g.cfg.ipd_mm > 20.0f && g.cfg.ipd_mm < 90.0f ? (double)g.cfg.ipd_mm / 1000.0 : 0.0635;
    d.render_scale = scale;
    d.version = ANLAND_XR_WIRE_VERSION;

    /* The trap this guards: the guest composes a single image whose aspect is
     * 2*eye_w : eye_h (SBS) or eye_w : 2*eye_h (SBS-V). If xr_eye_w/h was
     * overridden so that no longer matches the window, every frame gets
     * stretched instead of showing a resolution change. Say so once. */
    if (g.win_w && g.win_h && !g.aspect_warned) {
        double want = (double)d.eye_width / (double)d.eye_height;
        if (d.mode == ANLAND_XR_MODE_SBS_H) want *= 2.0;
        else if (d.mode == ANLAND_XR_MODE_SBS_V) want *= 0.5;
        const double have = (double)g.win_w / (double)g.win_h;
        if (fabs(want / have - 1.0) > 0.02) {
            LOGW("xr: eye target %ux%u (mode %u) spans %.2f:1 but the window is %ux%u"
                 " (%.2f:1) - the guest will stretch; leave xr_eye_w/h at 0 to follow"
                 " the window",
                 d.eye_width, d.eye_height, d.mode, want, g.win_w, g.win_h, have);
            g.aspect_warned = true;
        }
    }
}

/* Write anland-xr-display (tmp + rename: the driver polls it, it must never
 * see a half written file). */
void publish_display_locked(void) {
    char path[512], tmp[520];
    anland_xr_path(ANLAND_XR_DISPLAY_FILE, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        LOGE("xr: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    anland_xr_display_write(f, &g.disp);
    if (fclose(f) != 0) {
        LOGE("xr: flush %s: %s", tmp, strerror(errno));
        return;
    }
    chmod(tmp, 0666);
    if (rename(tmp, path) != 0) {
        LOGE("xr: rename %s: %s", path, strerror(errno));
        return;
    }
    LOGI("xr: display %ux%u/eye, mode=%u, %.1f Hz, fov=%.1f deg, ipd=%.1f mm",
         g.disp.eye_width, g.disp.eye_height, g.disp.mode, g.disp.refresh_hz,
         g.cfg.fov_deg, g.cfg.ipd_mm);
}

void publish_pose_locked(void) {
    if (!g_pose_map) return;
    volatile uint32_t* seq = (volatile uint32_t*)&g_pose_map->seq;
    anland_xr_seq_begin(seq);
    g_pose_map->magic = ANLAND_XR_POSE_MAGIC;
    struct anland_xr_pose& p = g_pose_map->pose;
    p.sample_ns = g.pose_ns;
    p.period_ns = g.period_ns;
    for (int i = 0; i < 4; i++) p.quat[i] = g.q[i];
    for (int i = 0; i < 3; i++) p.pos[i] = g.pos[i];
    p.ipd = g.disp.ipd;
    p.ang_vel[0] = g.ang_vel[0];
    p.ang_vel[1] = g.ang_vel[1];
    p.ang_vel[2] = g.ang_vel[2];
    p.tracking = 1;
    anland_xr_seq_end(seq);
}

void publish_input_locked(void) {
    if (!g_input_map) return;
    volatile uint32_t* seq = (volatile uint32_t*)&g_input_map->seq;
    anland_xr_seq_begin(seq);
    g_input_map->magic = ANLAND_XR_INPUT_MAGIC;
    g_input_map->input = g.input;
    anland_xr_seq_end(seq);
}

/* ------------------------------------------------------------------ */
/* GL pass                                                             */
/* ------------------------------------------------------------------ */

const char* kVs =
    "#version 300 es\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  /* one oversized triangle, no vertex buffer: p = (0,0) (2,0) (0,2) */\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  v_uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Panel pixel → tan space (the frustum the guest rendered), rotated by the
 * reprojection quaternion, back to a source uv. The barrel terms pre-distort
 * before sampling so the lens' own distortion lands the content on the
 * retina: k1/k2 are the coefficients of the lens (2nd/4th order in r²). */
const char* kFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_src;   /* u0, v0, su, sv */\n"
    "uniform vec4 u_fov;   /* left, right, top, bottom (tangents) */\n"
    "uniform vec2 u_k;     /* barrel k1, k2 */\n"
    "uniform mat3 u_warp;  /* eye-space rotation */\n"
    "uniform float u_ca;   /* chromatic aberration */\n"
    "uniform int u_mode;   /* 1 mono, 2 sbs-h, 3 sbs-v */\n"
    "out vec4 frag;\n"
    "vec2 tan_of(vec2 n) {\n"
    "  return vec2(mix(u_fov.x, u_fov.y, 0.5 + 0.5 * n.x),\n"
    "              mix(u_fov.w, u_fov.z, 0.5 + 0.5 * n.y));\n"
    "}\n"
    "void main() {\n"
    "  vec2 e = v_uv;\n"
    "  float eye = 0.0;\n"
    "  if (u_mode == 2) { eye = step(0.5, e.x); e.x = fract(e.x * 2.0); }\n"
    "  else if (u_mode == 3) { eye = step(0.5, e.y); e.y = fract(e.y * 2.0); }\n"
    "  vec2 n = e * 2.0 - 1.0;\n"
    "  float r2 = dot(n, n);\n"
    "  vec2 d = n * (1.0 + u_k.x * r2 + u_k.y * r2 * r2);\n"
    "  vec3 dir = vec3(tan_of(d), -1.0);\n"
    "  vec3 w = u_warp * dir;\n"
    "  vec2 t2 = vec2(w.x / -w.z, w.y / -w.z);\n"
    "  vec2 base;\n"
    "  base.x = (t2.x - u_fov.x) / (u_fov.y - u_fov.x);\n"
    "  base.y = (t2.y - u_fov.w) / (u_fov.z - u_fov.w);\n"
    "  if (u_mode == 2) base.x = (base.x + eye) * 0.5;\n"
    "  else if (u_mode == 3) base.y = (base.y + eye) * 0.5;\n"
    "  base.y = 1.0 - base.y;   /* source row 0 is the top */\n"
    "  base = clamp(base, 0.0, 1.0);\n"
    "  vec2 uv = u_src.xy + base * u_src.zw;\n"
    "  if (u_ca > 0.0) {\n"
    "    vec2 c = u_src.xy + u_src.zw * 0.5;\n"
    "    vec2 p = (uv - c) / max(u_src.zw, vec2(1e-4));\n"
    "    frag = vec4(texture(u_tex, c + u_src.zw * p * (1.0 + u_ca)).r,\n"
    "                texture(u_tex, uv).g,\n"
    "                texture(u_tex, c + u_src.zw * p * (1.0 - u_ca)).b,\n"
    "                1.0);\n"
    "  } else {\n"
    "    frag = vec4(texture(u_tex, uv).rgb, 1.0);\n"
    "  }\n"
    "}\n";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("xr: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* Lazily build the program on the render thread's context (the daemon never
 * creates a context of its own — the renderer's is the only one we get). */
bool ensure_gl(void) {
    if (g_prog) return true;
    if (g_gl_failed) return false;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        g_gl_failed = true;
        return false;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("xr: program link failed: %s", log);
        glDeleteProgram(p);
        g_gl_failed = true;
        return false;
    }
    g_prog = p;
    g_u_tex = glGetUniformLocation(p, "u_tex");
    g_u_src = glGetUniformLocation(p, "u_src");
    g_u_fov = glGetUniformLocation(p, "u_fov");
    g_u_k = glGetUniformLocation(p, "u_k");
    g_u_warp = glGetUniformLocation(p, "u_warp");
    g_u_ca = glGetUniformLocation(p, "u_ca");
    g_u_mode = glGetUniformLocation(p, "u_mode");
    glGenVertexArrays(1, &g_vao);
    LOGI("xr: distortion program ready");
    return true;
}

/* q = a ⊗ b (x, y, z, w). */
void quat_mul(const double a[4], const double b[4], double out[4]) {
    const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}

/* Android's game-rotation-vector is expressed in the ENU world frame (x east,
 * y north, z up). SteamVR's standing space is y up, -z forward. Both are
 * orthonormal frames, so a single fixed rotation takes one to the other:
 * -90° about x, applied on the left (it is a basis change of the target).
 * The rotation vector from SensorManager and the root IMU reader (same
 * convention on purpose) both go through here; an external provider is
 * documented to already speak the VR frame and is left alone. */
const double kEnuToVr[4] = {-0.70710678118654752, 0.0, 0.0, 0.70710678118654752};

void quat_enu_to_vr(const double q[4], double out[4]) {
    quat_mul(kEnuToVr, q, out);
}

/* Column-major 3x3 for GL from a unit quaternion. */
void quat_to_mat3(const double q[4], float m[9]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double wx = w * x, wy = w * y, wz = w * z;
    m[0] = (float)(1 - 2 * (yy + zz));
    m[1] = (float)(2 * (xy + wz));
    m[2] = (float)(2 * (xz - wy));
    m[3] = (float)(2 * (xy - wz));
    m[4] = (float)(1 - 2 * (xx + zz));
    m[5] = (float)(2 * (yz + wx));
    m[6] = (float)(2 * (xz + wy));
    m[7] = (float)(2 * (yz - wx));
    m[8] = (float)(1 - 2 * (xx + yy));
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool awl_xr_start(const awl_xr_config& cfg, uint32_t panel_w, uint32_t panel_h,
                  double refresh_hz) {
    std::lock_guard<std::mutex> lk(g.lock);
    g.cfg = cfg;
    g.panel_w = panel_w;
    g.panel_h = panel_h;
    g.panel_hz = refresh_hz > 1.0 ? refresh_hz : 72.0;
    if (g.started) {
        compute_display_locked();
        publish_display_locked();
        return true;
    }
    g_pose_map = (anland_xr_pose_block*)map_block(ANLAND_XR_POSE_FILE, &g_pose_fd);
    g_input_map = (anland_xr_input_block*)map_block(ANLAND_XR_INPUT_FILE, &g_input_fd);
    /* Optional 6DoF provider (ARCore activity, OpenXR bridge): whoever writes
     * this block owns position; the IMU session stays the fallback. */
    g_ext_map = (anland_xr_pose_block*)map_block_ro(ANLAND_XR_POSE_EXT_FILE, &g_ext_fd);
    compute_display_locked();
    publish_display_locked();
    /* a pose block that was never written must not look like a valid pose */
    if (g_pose_map) {
        g_pose_map->magic = 0;
        g_pose_map->seq = 0;
        g_pose_map->pose.tracking = 0;
    }
    if (g_input_map) {
        g_input_map->magic = 0;
        g_input_map->seq = 0;
    }
    g.started = true;
    LOGI("xr: started (enabled=%d mode=%d match=\"%s\")", g.cfg.enabled ? 1 : 0,
         g.cfg.mode, g.cfg.match);
    return true;
}

void awl_xr_apply(const awl_xr_config& cfg) {
    std::lock_guard<std::mutex> lk(g.lock);
    g.cfg = cfg;
    if (!g.started) return;
    compute_display_locked();
    publish_display_locked();
    publish_pose_locked();
    if (!cfg.enabled) g.windows.clear();
}

const awl_xr_config& awl_xr_get(void) { return g.cfg; }

bool awl_xr_enabled(void) {
    std::lock_guard<std::mutex> lk(g.lock);
    return g.cfg.enabled && g.cfg.mode >= 1;
}

void awl_xr_geometry(uint32_t* eye_w, uint32_t* eye_h, uint32_t* mode, double* refresh_hz) {
    std::lock_guard<std::mutex> lk(g.lock);
    if (eye_w) *eye_w = g.disp.eye_width;
    if (eye_h) *eye_h = g.disp.eye_height;
    if (mode) *mode = g.started ? g.disp.mode : 0;
    if (refresh_hz) *refresh_hz = g.disp.refresh_hz;
}

bool awl_xr_title_matches(const char* title) {
    if (!title || !title[0]) return false;
    char match[sizeof(g.cfg.match)];
    {
        std::lock_guard<std::mutex> lk(g.lock);
        memcpy(match, g.cfg.match, sizeof(match));
        match[sizeof(match) - 1] = 0;
    }
    char pat[sizeof(g.cfg.match)];
    memcpy(pat, match, sizeof(pat));
    pat[sizeof(pat) - 1] = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(pat, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t n = strlen(tok);
        while (n && (tok[n - 1] == ' ' || tok[n - 1] == '\t')) tok[--n] = 0;
        if (n && ci_contains(title, tok)) return true;
    }
    return false;
}

void awl_xr_mark_window(uint64_t id, bool xr) {
    std::lock_guard<std::mutex> lk(g.lock);
    if (xr) g.windows[id] = true;
    else g.windows.erase(id);
}

bool awl_xr_window_active(uint64_t id) {
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.cfg.enabled || g.cfg.mode < 1) return false;
    auto it = g.windows.find(id);
    return it != g.windows.end() && it->second;
}

bool awl_xr_pose_valid(void) {
    std::lock_guard<std::mutex> lk(g.lock);
    return g.pose_ok && now_ns() - g.pose_ns < kPoseFreshNs;
}

/* Adopt a fresher pose from the optional external provider (anland-xr-pose-ext).
 * That file is the plug-in point for real 6DoF: an ARCore activity on a phone,
 * or an OpenXR bridge on the headset, can publish position and orientation
 * there and the daemon will prefer it over the IMU session's 3DoF. Caller holds
 * g.lock. */
void maybe_take_external_locked(void) {
    if (!g_ext_map) return;
    anland_xr_pose p;
    if (!read_pose_block(g_ext_map, &p)) return;
    if (p.sample_ns <= g.pose_ns) return;
    double q[4] = {p.quat[0], p.quat[1], p.quat[2], p.quat[3]};
    const double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6) return;
    for (int i = 0; i < 4; i++) q[i] /= n;

    double prev[4];
    for (int i = 0; i < 4; i++) prev[i] = g.q[i];
    const double dt = (double)(p.sample_ns - g.pose_ns) * 1e-9;

    for (int i = 0; i < 4; i++) g.q[i] = q[i];
    for (int i = 0; i < 3; i++) g.pos[i] = p.pos[i];

    const bool has_vel = fabs(p.ang_vel[0]) + fabs(p.ang_vel[1]) + fabs(p.ang_vel[2]) > 1e-9;
    if (has_vel) {
        for (int i = 0; i < 3; i++) g.ang_vel[i] = p.ang_vel[i];
    } else if (dt > 1e-6) {
        quat_to_ang_vel(prev, q, dt, g.ang_vel);
    }
    g.pose_ns = p.sample_ns;
    g.period_ns = (uint32_t)p.period_ns;
    g.pose_ok = true;
    g.ext_ns = p.sample_ns;
    if (!g.ext_logged) {
        g.ext_logged = true;
        LOGI("xr: external pose provider is live (%s) - IMU poses paused", ANLAND_XR_POSE_EXT_FILE);
    }
    publish_pose_locked();
}

/* True while an external provider is still delivering poses. */
bool ext_fresh_locked(void) {
    if (!g.ext_ns) return false;
    const uint64_t now = now_ns();
    return now > g.ext_ns && (now - g.ext_ns) < kPoseFreshNs;
}

/* True while the daemon's own IMU reader is still delivering poses. */
bool imu_fresh_locked(void) {
    if (!g.imu_ns) return false;
    const uint64_t now = now_ns();
    return now > g.imu_ns && (now - g.imu_ns) < kPoseFreshNs;
}

void set_pose_common(const float quat[4], const float pos[3], const float ang_vel[3],
                     uint64_t sample_ns, uint32_t period_ns) {
    std::lock_guard<std::mutex> lk(g.lock);
    maybe_take_external_locked();
    /* an external provider owns the pose while it is live */
    if (ext_fresh_locked()) return;
    /* the root IMU reader outranks the Activity: it samples raw channels at a
     * higher rate and does not depend on the Activity being foreground */
    if (imu_fresh_locked()) return;
    double q[4] = {quat[0], quat[1], quat[2], quat[3]};
    /* normalize (binder floats are not guaranteed unit length) */
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6) { q[0] = q[1] = q[2] = 0.0; q[3] = 1.0; }
    else for (int i = 0; i < 4; i++) q[i] /= n;
    {
        /* the Activity sends SensorManager's ENU pose */
        double vr[4];
        quat_enu_to_vr(q, vr);
        for (int i = 0; i < 4; i++) q[i] = vr[i];
    }
    const uint64_t ns = sample_ns ? sample_ns : now_ns();
    if (ang_vel) {
        for (int i = 0; i < 3; i++) g.ang_vel[i] = ang_vel[i];
    } else {
        double w[3];
        quat_to_ang_vel(g.q, q, (double)(ns - g.pose_ns) * 1e-9, w);
        for (int i = 0; i < 3; i++) g.ang_vel[i] = w[i];
    }
    for (int i = 0; i < 4; i++) g.q[i] = q[i];
    for (int i = 0; i < 3; i++) g.pos[i] = pos ? pos[i] : 0.0;
    g.pose_ns = ns;
    g.period_ns = period_ns;
    g.pose_ok = true;
    publish_pose_locked();
}

void awl_xr_set_pose(const float quat[4], const float pos[3], uint64_t sample_ns,
                     uint32_t period_ns) {
    set_pose_common(quat, pos, nullptr, sample_ns, period_ns);
}

void awl_xr_set_pose_vel(const float quat[4], const float pos[3], const float ang_vel[3],
                         uint64_t sample_ns, uint32_t period_ns) {
    set_pose_common(quat, pos, ang_vel, sample_ns, period_ns);
}

void awl_xr_submit_imu(const float quat[4], const float ang_vel[3], uint64_t sample_ns,
                       uint32_t period_ns) {
    if (!quat) return;
    std::lock_guard<std::mutex> lk(g.lock);
    if (!g.cfg.enabled || g.cfg.mode < 1) return;
    /* An external 6DoF provider still wins: it knows things the IMU cannot
     * (translation), and mixing the two would make the reprojection jitter. */
    if (ext_fresh_locked()) return;
    if (sample_ns && g.imu_ns && sample_ns <= g.imu_ns) return;

    double q[4] = {quat[0], quat[1], quat[2], quat[3]};
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6) { q[0] = q[1] = q[2] = 0.0; q[3] = 1.0; }
    else for (int i = 0; i < 4; i++) q[i] /= n;
    {
        /* awl_imu integrates in the same ENU frame the Activity reports */
        double vr[4];
        quat_enu_to_vr(q, vr);
        for (int i = 0; i < 4; i++) q[i] = vr[i];
    }

    for (int i = 0; i < 4; i++) g.q[i] = q[i];
    if (ang_vel) for (int i = 0; i < 3; i++) g.ang_vel[i] = ang_vel[i];
    /* root IMU is 3DoF: no translation, so the position stays where it was */
    g.pose_ns = sample_ns ? sample_ns : now_ns();
    g.imu_ns = g.pose_ns;
    g.period_ns = period_ns;
    g.pose_ok = true;
    if (!g.imu_logged) {
        g.imu_logged = true;
        LOGI("xr: root IMU provider is live - Activity poses are paused");
    }
    publish_pose_locked();
}

void awl_xr_set_input(const int32_t* v, size_t n) {
    if (!v || n < 1) return;
    std::lock_guard<std::mutex> lk(g.lock);
    const int32_t nc = v[0] < 0 ? 0 : (v[0] > ANLAND_XR_MAX_CONTROLLERS ? ANLAND_XR_MAX_CONTROLLERS : v[0]);
    /* layout (ints): n, then per controller 9 fields:
     * present, buttons, trigger, grip, stick_x, stick_y, trackpad_x,
     * trackpad_y, pad_pressed — scalars are q1e4. */
    const size_t per = 9;
    anland_xr_input in;
    memset(&in, 0, sizeof(in));
    in.sample_ns = now_ns();
    in.n = (uint32_t)nc;
    for (int32_t i = 0; i < nc; i++) {
        const size_t base = 1 + (size_t)i * per;
        if (base + per > n) { in.n = (uint32_t)i; break; }
        anland_xr_controller& c = in.c[i];
        c.present = v[base] ? 1u : 0u;
        c.buttons = (uint32_t)v[base + 1];
        c.trigger = (float)v[base + 2] / 10000.0f;
        c.grip = (float)v[base + 3] / 10000.0f;
        c.stick_x = (float)v[base + 4] / 10000.0f;
        c.stick_y = (float)v[base + 5] / 10000.0f;
        c.trackpad_x = (float)v[base + 6] / 10000.0f;
        c.trackpad_y = (float)v[base + 7] / 10000.0f;
        c.pad_pressed = (float)v[base + 8] / 10000.0f;
        c.quat[3] = 1.0; /* 3DoF controllers are not tracked yet */
    }
    g.input = in;
    publish_input_locked();
}

void awl_xr_note_render(uint64_t id) {
    std::lock_guard<std::mutex> lk(g.lock);
    state::window_ref& ref = g.refs[id];
    for (int i = 0; i < 4; i++) ref.q[i] = g.q[i];
}

void awl_xr_note_window_size(uint64_t id, uint32_t w, uint32_t h) {
    if (w < 64 || h < 64) return;
    std::lock_guard<std::mutex> lk(g.lock);
    if (g.win_w == w && g.win_h == h) return;
    /* Only XR windows describe the render target; other windows' sizes are
     * noise here. The width/height we keep is the largest XR window seen, so a
     * transient small surface cannot shrink the contract. */
    auto it = g.windows.find(id);
    if (it == g.windows.end() || !it->second) return;
    if (w * h < g.win_w * (uint64_t)g.win_h) return;
    g.win_w = w;
    g.win_h = h;
    g.aspect_warned = false;
    compute_display_locked();
    publish_display_locked();
    LOGI("xr: render target follows the window: %ux%u", w, h);
}

void awl_xr_draw(uint64_t id, uint32_t tex, uint32_t bw, uint32_t bh, int vw, int vh,
                 float u0, float v0, float su, float sv) {
    (void)bw;
    (void)bh;
    if (vw <= 0 || vh <= 0 || !tex) return;
    if (!ensure_gl()) return;

    awl_xr_config cfg;
    anland_xr_display disp;
    double q_now[4], q_prev[4];
    bool fresh;
    {
        std::lock_guard<std::mutex> lk(g.lock);
        maybe_take_external_locked();
        cfg = g.cfg;
        disp = g.disp;
        const uint64_t age = now_ns() - g.pose_ns;
        fresh = g.pose_ok && age < kPoseFreshNs;

        /* q_now is the pose of *this* window's last drawn frame (or the current
         * pose when the window has not drawn yet), extrapolated forward: the
         * head keeps moving between the sensor sample and the photon, so with
         * xr_predict > 0 we add that gap back on top of the sample age. */
        auto it = g.refs.find(id);
        const double* ref = it != g.refs.end() ? it->second.q : g.q;
        for (int i = 0; i < 4; i++) { q_prev[i] = ref[i]; q_now[i] = g.q[i]; }
        if (fresh && cfg.predict_ms > 0) {
            const double dt = (double)(age + (uint64_t)cfg.predict_ms * 1000000ull) * 1e-9;
            const double r[3] = {g.ang_vel[0] * dt, g.ang_vel[1] * dt, g.ang_vel[2] * dt};
            double step[4], out[4];
            quat_from_scaled_axis(r, step);
            quat_mul(g.q, step, out);
            for (int i = 0; i < 4; i++) q_now[i] = out[i];
        }
    }

    /* Reprojection: R = q_now ⊗ conj(q_prev) — the rotation the head made
     * since the frame it is showing was composed. */
    float warp[9];
    if (fresh) {
        double c[4] = {-q_prev[0], -q_prev[1], -q_prev[2], q_prev[3]};
        double rel[4];
        quat_mul(q_now, c, rel);
        quat_to_mat3(rel, warp);
    } else {
        /* no fresh pose (no IMU session attached, or the Activity paused):
         * show the frame unwarped instead of guessing */
        const double id[4] = {0, 0, 0, 1};
        quat_to_mat3(id, warp);
    }

    const float fov_l = (float)disp.fov_left, fov_r = (float)disp.fov_right;
    const float fov_t = (float)disp.fov_top, fov_b = (float)disp.fov_bottom;

    glUseProgram(g_prog);
    glDisable(GL_BLEND);
    glBindVertexArray(g_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glUniform1i(g_u_tex, 0);
    glUniform4f(g_u_src, u0, v0, su, sv);
    glUniform4f(g_u_fov, fov_l, fov_r, fov_t, fov_b);
    glUniform2f(g_u_k, cfg.k1, cfg.k2);
    glUniformMatrix3fv(g_u_warp, 1, GL_FALSE, warp);
    glUniform1f(g_u_ca, cfg.ca > 0.0f ? cfg.ca : 0.0f);
    glUniform1i(g_u_mode, disp.mode < 1 ? 1 : (int)disp.mode);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);

    {
        std::lock_guard<std::mutex> lk(g.lock);
        /* Reference for the *next* frame of this window: the pose the guest's
         * content actually corresponds to, i.e. the unextrapolated one. */
        state::window_ref& ref = g.refs[id];
        for (int i = 0; i < 4; i++) ref.q[i] = g.q[i];
    }
}

void awl_xr_shutdown(void) {
    if (g_pose_map) munmap(g_pose_map, ANLAND_XR_BLOCK_BYTES);
    if (g_input_map) munmap(g_input_map, ANLAND_XR_BLOCK_BYTES);
    if (g_ext_map) munmap(g_ext_map, ANLAND_XR_BLOCK_BYTES);
    if (g_pose_fd >= 0) close(g_pose_fd);
    if (g_input_fd >= 0) close(g_input_fd);
    if (g_ext_fd >= 0) close(g_ext_fd);
    g_pose_map = nullptr;
    g_input_map = nullptr;
    g_ext_map = nullptr;
    g_pose_fd = g_input_fd = g_ext_fd = -1;
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_prog) glDeleteProgram(g_prog);
    g_vao = 0;
    g_prog = 0;
}
