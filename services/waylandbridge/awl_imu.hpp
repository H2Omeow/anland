/* Root-side IMU reader (docs/vr.md, "Root, sensors and what they buy you").
 *
 * anland's daemon runs as root, so it does not have to ask an Activity for an
 * already-fused orientation through SensorManager: it can read the raw
 * gyroscope and accelerometer channels straight out of sysfs, at whatever rate
 * the hardware exposes, and fuse them itself. That removes three of the app
 * path's properties at once — the 2D-app sensor restrictions of locked-down
 * builds, the fused-rate ceiling around 100 Hz, and the extra binder hop.
 *
 * What it does not remove: 3DoF is still 3DoF. There is no positional tracking
 * here, because there is nothing in sysfs that measures translation. A 6DoF
 * provider still has to publish anland-xr-pose-ext (which then wins over this
 * module — see awl_xr.cpp).
 */
#pragma once

#include <stdint.h>

struct awl_imu_config {
    int enabled = 0;      /* xr_imu: 0 = off, 1 = read the sensors directly */
    int rate_hz = 200;    /* xr_imu_hz: sensor polling rate */
    /* xr_imu_gyro / xr_imu_accel: "/sys/bus/iio/devices/iio:device2",
     * "/dev/iio:device2", "iio:device2" or a name substring ("lsm6dso").
     * Empty = auto-detect. */
    char gyro[128] = {0};
    char accel[128] = {0};
};

/* Applies cfg; starts the reader thread on first use. Returns false only when
 * the configuration is unusable (it still retries later: sensors can appear
 * after the daemon started, e.g. a kernel module finishing probe). */
bool awl_imu_apply(const awl_imu_config& cfg);

/* True while samples are still arriving (the reader found a device). */
bool awl_imu_live(void);

/* Human-readable description of the active source, for logs and settings. */
const char* awl_imu_source(void);

void awl_imu_shutdown(void);
