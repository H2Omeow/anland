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
    uint64_t pose_ns = 0;
    uint32_t period_ns = 0;
    bool pose_ok = false;

    /* pose the previous XR frame was drawn with (reprojection reference) */
    double last_q[4] = {0.0, 0.0, 0.0, 1.0};

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
anland_xr_pose_block* g_pose_map = nullptr;
anland_xr_input_block* g_input_map = nullptr;

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

    uint32_t ew = (uint32_t)g.cfg.eye_w > 0 ? (uint32_t)g.cfg.eye_w
                                            : (d.mode == ANLAND_XR_MODE_SBS_H ? g.panel_w / 2 : g.panel_w);
    uint32_t eh = (uint32_t)g.cfg.eye_h > 0 ? (uint32_t)g.cfg.eye_h
                                            : (d.mode == ANLAND_XR_MODE_SBS_V ? g.panel_h / 2 : g.panel_h);
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

void awl_xr_set_pose(const float quat[4], const float pos[3], uint64_t sample_ns,
                     uint32_t period_ns) {
    std::lock_guard<std::mutex> lk(g.lock);
    double q[4] = {quat[0], quat[1], quat[2], quat[3]};
    /* normalize (binder floats are not guaranteed unit length) */
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6) { q[0] = q[1] = q[2] = 0.0; q[3] = 1.0; }
    else for (int i = 0; i < 4; i++) q[i] /= n;
    for (int i = 0; i < 4; i++) g.q[i] = q[i];
    for (int i = 0; i < 3; i++) g.pos[i] = pos ? pos[i] : 0.0;
    g.pose_ns = sample_ns ? sample_ns : now_ns();
    g.period_ns = period_ns;
    g.pose_ok = true;
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

void awl_xr_note_render(void) {
    std::lock_guard<std::mutex> lk(g.lock);
    for (int i = 0; i < 4; i++) g.last_q[i] = g.q[i];
}

void awl_xr_draw(uint32_t tex, uint32_t bw, uint32_t bh, int vw, int vh,
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
        cfg = g.cfg;
        disp = g.disp;
        for (int i = 0; i < 4; i++) { q_now[i] = g.q[i]; q_prev[i] = g.last_q[i]; }
        fresh = g.pose_ok && now_ns() - g.pose_ns < kPoseFreshNs;
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
        for (int i = 0; i < 4; i++) g.last_q[i] = g.q[i];
    }
}

void awl_xr_shutdown(void) {
    if (g_pose_map) munmap(g_pose_map, ANLAND_XR_BLOCK_BYTES);
    if (g_input_map) munmap(g_input_map, ANLAND_XR_BLOCK_BYTES);
    if (g_pose_fd >= 0) close(g_pose_fd);
    if (g_input_fd >= 0) close(g_input_fd);
    g_pose_map = nullptr;
    g_input_map = nullptr;
    g_pose_fd = g_input_fd = -1;
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_prog) glDeleteProgram(g_prog);
    g_vao = 0;
    g_prog = 0;
}
