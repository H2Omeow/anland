/* Root-side IMU reader — see awl_imu.hpp for why this exists.
 *
 * Reads the raw channels from IIO sysfs. One `read()` per axis per tick, so a
 * 200 Hz loop costs ~1200 syscalls/s: nothing, and it works on every kernel
 * that exposes the channels whether or not it also exposes a buffer interface.
 * The fusion below is deliberately the same shape as the Activity's fallback
 * (gyro integration + gravity tilt correction), just at a higher rate and with
 * a gyro-bias estimate, so switching providers does not change how the world
 * moves — only how steady it is.
 */

#define AWL_TAG "anland-imu"
#include "awl_log.h"

#include "awl_imu.hpp"
#include "awl_xr.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kGravity = 9.80665;

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

/* One scalar sysfs file read as a double. Sysfs returns the whole value on
 * every read, so rewinding before each read is all the state a channel needs. */
bool read_num(const char* path, double* out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char b[64];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return false;
    b[n] = 0;
    char* end = nullptr;
    const double v = strtod(b, &end);
    if (end == b) return false;
    *out = v;
    return true;
}

bool read_text(const char* path, char* out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t got = read(fd, out, n - 1);
    close(fd);
    if (got <= 0) return false;
    out[got] = 0;
    char* nl = strchr(out, '\n');
    if (nl) *nl = 0;
    return true;
}

/* ---------- device selection ---------- */

struct src {
    int fd[3] = {-1, -1, -1};
    double scale = 1.0;
    char dir[256] = {0};
    char name[64] = {0};
    bool ok = false;
};

void close_src(src& s) {
    for (int i = 0; i < 3; i++) {
        if (s.fd[i] >= 0) close(s.fd[i]);
        s.fd[i] = -1;
    }
    s.ok = false;
}

bool has_channels(const std::string& dir, const char* kind) {
    char p[512];
    snprintf(p, sizeof(p), "%s/in_%s_x_raw", dir.c_str(), kind);
    return access(p, R_OK) == 0;
}

std::string name_of(const std::string& dir) {
    char p[512], nm[64] = "";
    snprintf(p, sizeof(p), "%s/name", dir.c_str());
    if (!read_text(p, nm, sizeof(nm))) return std::string();
    return std::string(nm);
}

/* "/sys/...", "/dev/iio:deviceN", "iio:deviceN" → a sysfs directory.
 * Anything else is treated as a name hint by the scanner. */
bool dir_from_hint(const char* hint, std::string* out) {
    if (!hint || !hint[0]) return false;
    const std::string s(hint);
    if (s.rfind("/sys/", 0) == 0) { *out = s; return true; }
    if (s.rfind("/dev/", 0) == 0) { *out = "/sys/bus/iio/devices/" + s.substr(5); return true; }
    if (s.rfind("iio:", 0) == 0) { *out = "/sys/bus/iio/devices/" + s; return true; }
    return false;
}

void scan_dirs(std::vector<std::string>* out) {
    DIR* d = opendir("/sys/bus/iio/devices");
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "iio:device", 10) != 0) continue;
        out->push_back(std::string("/sys/bus/iio/devices/") + e->d_name);
    }
    closedir(d);
    std::sort(out->begin(), out->end());
}

bool use_hint(const std::string& dir, const char* hint) {
    if (!hint || !hint[0]) return true;
    std::string h;
    if (dir_from_hint(hint, &h)) return dir == h;
    return ci_contains(name_of(dir).c_str(), hint) || ci_contains(dir.c_str(), hint);
}

/* Auto-detect: prefer one chip carrying both channels (the common case: an
 * LSM6DSO/BMI260 with gyro and accel on the same IIO device), then fall back to
 * separate devices for gyro and accel. */
bool pick_dirs(const awl_imu_config& cfg, std::string* gdir, std::string* adir) {
    std::string g, a;
    dir_from_hint(cfg.gyro, &g);
    dir_from_hint(cfg.accel, &a);
    if (!g.empty() && !has_channels(g, "anglvel")) g.clear();
    if (!a.empty() && !has_channels(a, "accel")) a.clear();

    std::vector<std::string> dirs;
    scan_dirs(&dirs);

    if (g.empty() || a.empty()) {
        for (const std::string& d : dirs) {
            const bool dg = has_channels(d, "anglvel");
            const bool da = has_channels(d, "accel");
            if (!dg || !da) continue;
            if (!use_hint(d, cfg.gyro) || !use_hint(d, cfg.accel)) continue;
            if (g.empty()) g = d;
            if (a.empty()) a = d;
            break;
        }
    }
    if (g.empty())
        for (const std::string& d : dirs)
            if (has_channels(d, "anglvel") && use_hint(d, cfg.gyro)) { g = d; break; }
    if (a.empty())
        for (const std::string& d : dirs)
            if (has_channels(d, "accel") && use_hint(d, cfg.accel)) { a = d; break; }

    if (g.empty() || a.empty()) return false;
    *gdir = g;
    *adir = a;
    return true;
}

bool open_src(const std::string& dir, const char* kind, double default_scale, src& s) {
    close_src(s);
    snprintf(s.dir, sizeof(s.dir), "%s", dir.c_str());
    snprintf(s.name, sizeof(s.name), "%s", name_of(dir).c_str());

    double scale = default_scale;
    char p[512];
    snprintf(p, sizeof(p), "%s/in_%s_scale", dir.c_str(), kind);
    double v = 0.0;
    if (read_num(p, &v) && v > 0.0) scale = v;
    else LOGW("imu: %s has no in_%s_scale - assuming %g (~default full scale)",
              dir.c_str(), kind, default_scale);
    s.scale = scale;

    static const char* kAxes[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), "%s/in_%s_%s_raw", dir.c_str(), kind, kAxes[i]);
        s.fd[i] = open(p, O_RDONLY | O_CLOEXEC);
        if (s.fd[i] < 0) {
            LOGE("imu: open %s: %s", p, strerror(errno));
            close_src(s);
            return false;
        }
    }
    s.ok = true;
    return true;
}

/* Best-effort: ask the chip for the rate we poll at, so the internal ODR is not
 * the limiting factor. Permission/absence is fine — raw reads still work. */
void try_set_rate(const std::string& dir, int hz) {
    char p[512];
    snprintf(p, sizeof(p), "%s/sampling_frequency", dir.c_str());
    if (access(p, W_OK) != 0) return;
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char b[16];
    const int n = snprintf(b, sizeof(b), "%d", hz);
    if (write(fd, b, (size_t)n) != n) LOGD("imu: sampling_frequency=%d not accepted", hz);
    close(fd);
}

bool read_axis(int fd, double* out) {
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
    char b[32];
    const ssize_t n = read(fd, b, sizeof(b) - 1);
    if (n <= 0) return false;
    b[n] = 0;
    char* end = nullptr;
    const double v = strtod(b, &end);
    if (end == b) return false;
    *out = v;
    return true;
}

/* ---------- quaternion helpers (x,y,z,w; same convention as the wire) ---------- */

void q_mul(const double a[4], const double b[4], double out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void q_normalize(double q[4]) {
    const double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-9) { q[0] = q[1] = q[2] = 0.0; q[3] = 1.0; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}

void q_from_scaled_axis(const double r[3], double out[4]) {
    const double angle = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (angle < 1e-9) { out[0] = out[1] = out[2] = 0.0; out[3] = 1.0; return; }
    const double s = sin(angle * 0.5) / angle;
    out[0] = r[0] * s;
    out[1] = r[1] * s;
    out[2] = r[2] * s;
    out[3] = cos(angle * 0.5);
}

/* Rotate a body vector into the world frame (world: X east, Y north, Z up —
 * the same frame SensorManager's game rotation vector reports, which is what
 * the Activity path sends, so the two providers stay interchangeable). */
void q_rotate(const double q[4], const double v[3], double out[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2.0 * (y * v[2] - z * v[1]);
    const double ty = 2.0 * (z * v[0] - x * v[2]);
    const double tz = 2.0 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* ---------- reader thread ---------- */

struct runtime {
    std::mutex m;
    std::condition_variable cv;
    awl_imu_config want;
    unsigned want_gen = 0;
    bool stop = false;
    bool started = false;
    std::thread th;
    std::atomic<bool> live{false};
    std::atomic<uint64_t> last_ns{0};
    std::mutex src_m;
    char source[256] = "off";
};

runtime rt;

void set_source(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(rt.src_m);
        memcpy(rt.source, buf, sizeof(rt.source));
        rt.source[sizeof(rt.source) - 1] = 0;
    }
}

void reader_loop(void) {
    src gyro, accel;
    awl_imu_config active;
    bool have_cfg = false;
    unsigned applied_gen = 0;
    double q[4] = {0.0, 0.0, 0.0, 1.0};
    double bias[3] = {0.0, 0.0, 0.0};
    uint64_t prev_ns = 0;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(rt.m);
            if (rt.stop) break;
            /* Only a generation bump means the configuration changed; the loop
             * otherwise keeps its open channel fds across ticks. */
            if (have_cfg && rt.want_gen == applied_gen) {
                lk.unlock();
            } else {
                active = rt.want;
                applied_gen = rt.want_gen;
                have_cfg = true;
                lk.unlock();
                close_src(gyro);
                close_src(accel);
                prev_ns = 0;
                memset(bias, 0, sizeof(bias));
                q[0] = q[1] = q[2] = 0.0;
                q[3] = 1.0;
                rt.live.store(false, std::memory_order_relaxed);
                if (!active.enabled) {
                    set_source("off (xr_imu=0)");
                } else {
                    std::string gd, ad;
                    if (!pick_dirs(active, &gd, &ad)) {
                        set_source("no iio device with gyro+accel channels");
                        LOGW("imu: no usable iio device (looked for in_anglvel_*_raw and "
                             "in_accel_*_raw under /sys/bus/iio/devices) - retrying");
                    } else if (!open_src(gd, "anglvel", 0.001, gyro) ||
                               !open_src(ad, "accel", 0.0005987548828125, accel)) {
                        close_src(gyro);
                        close_src(accel);
                        set_source("iio channels vanished");
                    } else {
                        try_set_rate(gd, active.rate_hz);
                        try_set_rate(ad, active.rate_hz);
                        set_source("%s/%s @%dHz", gyro.name[0] ? gyro.name : gd.c_str(),
                                   accel.name[0] ? accel.name : ad.c_str(), active.rate_hz);
                        LOGI("imu: reading %s (gyro) and %s (accel) at %d Hz, scales %.6g / %.6g",
                             gd.c_str(), ad.c_str(), active.rate_hz, gyro.scale, accel.scale);
                    }
                }
            }
        }

        const int hz = active.rate_hz >= 50 ? active.rate_hz : 200;
        const uint64_t period = 1000000000ull / (uint64_t)hz;

        if (!active.enabled || !gyro.ok || !accel.ok) {
            std::unique_lock<std::mutex> lk(rt.m);
            rt.cv.wait_for(lk, std::chrono::milliseconds(active.enabled ? 1000 : 500));
            continue;
        }

        double gr[3], ar[3];
        bool good = true;
        for (int i = 0; i < 3; i++) {
            if (!read_axis(gyro.fd[i], &gr[i]) || !read_axis(accel.fd[i], &ar[i])) {
                good = false;
                break;
            }
        }
        if (!good) {
            LOGW("imu: channel read failed - re-detecting");
            close_src(gyro);
            close_src(accel);
            rt.live.store(false, std::memory_order_relaxed);
            set_source("sensor read failed");
            std::unique_lock<std::mutex> lk(rt.m);
            rt.cv.wait_for(lk, std::chrono::milliseconds(500));
            continue;
        }

        const uint64_t ns = now_ns();
        double g[3] = {gr[0] * gyro.scale, gr[1] * gyro.scale, gr[2] * gyro.scale};
        double a[3] = {ar[0] * accel.scale, ar[1] * accel.scale, ar[2] * accel.scale};
        const double dt = prev_ns ? (double)(ns - prev_ns) * 1e-9 : 1.0 / (double)hz;
        prev_ns = ns;
        if (dt <= 0.0 || dt > 0.25) {
            /* first sample, or we were descheduled: no integration this tick */
        } else {
            /* Gyro bias: the chip's zero-rate offset is what makes a still head
             * drift. Estimate it only while the device looks still, and only
             * slowly, so real motion never trains itself in as bias. */
            const double amag = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const double gmag = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            if (fabs(amag - kGravity) < 0.35 && gmag < 0.08) {
                const double k = 0.002;
                for (int i = 0; i < 3; i++) bias[i] += (g[i] - bias[i]) * k;
            }

            double w[3] = {g[0] - bias[0], g[1] - bias[1], g[2] - bias[2]};
            const double r[3] = {w[0] * dt * 0.5, w[1] * dt * 0.5, w[2] * dt * 0.5};
            double dq[4], nq[4];
            q_from_scaled_axis(r, dq);
            q_mul(q, dq, nq);
            for (int i = 0; i < 4; i++) q[i] = nq[i];
            q_normalize(q);

            /* Tilt: gravity is a long-term truth the gyro cannot contradict.
             * Rotate the body frame so the predicted up axis walks back onto
             * the measured one (gain scales with dt → same time constant at any
             * rate). */
            if (fabs(amag - kGravity) < 1.0) {
                double up_meas[3] = {a[0] / amag, a[1] / amag, a[2] / amag};
                const double zaxis[3] = {0.0, 0.0, 1.0};
                double up_pred[3];
                q_rotate(q, zaxis, up_pred);
                double cross[3] = {
                    up_pred[1] * up_meas[2] - up_pred[2] * up_meas[1],
                    up_pred[2] * up_meas[0] - up_pred[0] * up_meas[2],
                    up_pred[0] * up_meas[1] - up_pred[1] * up_meas[0],
                };
                const double s = sqrt(cross[0] * cross[0] + cross[1] * cross[1] +
                                      cross[2] * cross[2]);
                const double c = up_pred[0] * up_meas[0] + up_pred[1] * up_meas[1] +
                                 up_pred[2] * up_meas[2];
                if (s > 1e-9) {
                    const double angle = atan2(s, c);
                    double gain = dt * 2.0;
                    if (gain > 0.2) gain = 0.2;
                    const double ax[3] = {cross[0] / s, cross[1] / s, cross[2] / s};
                    const double rr[3] = {ax[0] * angle * gain, ax[1] * angle * gain,
                                          ax[2] * angle * gain};
                    double cdq[4], cq[4];
                    q_from_scaled_axis(rr, cdq);
                    q_mul(q, cdq, cq);
                    for (int i = 0; i < 4; i++) q[i] = cq[i];
                    q_normalize(q);
                }
            }

            const float fq[4] = {(float)q[0], (float)q[1], (float)q[2], (float)q[3]};
            const float fw[3] = {(float)w[0], (float)w[1], (float)w[2]};
            awl_xr_submit_imu(fq, fw, ns, (uint32_t)period);
            rt.last_ns.store(ns, std::memory_order_relaxed);
            rt.live.store(true, std::memory_order_relaxed);
        }

        std::unique_lock<std::mutex> lk(rt.m);
        rt.cv.wait_for(lk, std::chrono::nanoseconds(period));
    }

    close_src(gyro);
    close_src(accel);
    rt.live.store(false, std::memory_order_relaxed);
}

} /* namespace */

bool awl_imu_apply(const awl_imu_config& cfg) {
    std::lock_guard<std::mutex> lk(rt.m);
    rt.want = cfg;
    rt.want_gen++;
    if (!rt.started) {
        rt.started = true;
        rt.th = std::thread(reader_loop);
        LOGI("imu: reader thread started (enabled=%d rate=%dHz gyro=\"%s\" accel=\"%s\")",
             cfg.enabled, cfg.rate_hz, cfg.gyro, cfg.accel);
    }
    rt.cv.notify_all();
    return true;
}

bool awl_imu_live(void) {
    const uint64_t last = rt.last_ns.load(std::memory_order_relaxed);
    if (!last) return false;
    const uint64_t now = now_ns();
    return now > last && (now - last) < 500ull * 1000ull * 1000ull;
}

const char* awl_imu_source(void) {
    static char buf[256];
    {
        std::lock_guard<std::mutex> lk(rt.src_m);
        memcpy(buf, rt.source, sizeof(buf));
        buf[sizeof(buf) - 1] = 0;
    }
    return buf;
}

void awl_imu_shutdown(void) {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(rt.m);
        if (!rt.started) return;
        rt.stop = true;
        rt.cv.notify_all();
        th = std::move(rt.th);
    }
    if (th.joinable()) th.join();
    rt.started = false;
}
