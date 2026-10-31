// driver_anland.cpp —OpenVR driver for anland's XR windows.
// ============================================================================
// What this driver is
// -------------------
// anland presents a Linux app's windows as Android windows. A window whose
// title matches the daemon's `xr_match` (default: SteamVR/vrcompositor) is
// composited by the daemon as an XR surface: per-eye barrel pre-distortion
// plus timewarp reprojection, filling the phone/glasses panel. This driver
// gives SteamVR the HMD it needs for that mode, and nothing else:
//
//   * one HMD that reports no lens distortion (the daemon does the warping),
//     a per-eye target size, FOV/IPD from the daemon's display file, and a
//     pose streamed from the phone's IMU (3DoF);
//   * two controllers that report the head pose plus a fixed shoulder offset
//     and expose the buttons/sticks of the gamepads the Android Activity sees.
//
// The pose/input data path is: Android IMU →binder →anland daemon →// /run/anland/anland-xr-pose (seqlock) →this driver. The geometry path is:
// daemon →/run/anland/anland-xr-display (rewritten on every config change).
//
// Why 3DoF: phones and AR glasses without SLAM have no positional tracking,
// and SteamVR accepts a 3DoF HMD (it is what phone-based headsets did for
// years). Controllers are not independently tracked either —faking 6DoF
// would make every game feel broken, so their pose rides the head.
//
// Build: xr/build.sh (aarch64 Linux, g++ -shared). Install: xr/anland-steamvr.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "openvr_driver.h"
#include "anland_xr_wire.h"   /* xr/include —shared with the daemon */

using namespace vr;

static const char* kLogTag = "[anland] ";

/* ------------------------------------------------------------------------- */
/* shared-file readers                                                       */
/* ------------------------------------------------------------------------- */

namespace {

struct PoseFile {
    int fd = -1;
    const anland_xr_pose_block* block = nullptr;

    void open_file() {
        if (block) return;
        char path[512];
        if (anland_xr_path(ANLAND_XR_POSE_FILE, path, sizeof(path)) != 0) return;
        fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        void* p = ::mmap(nullptr, ANLAND_XR_BLOCK_BYTES, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            ::close(fd);
            fd = -1;
            return;
        }
        block = static_cast<const anland_xr_pose_block*>(p);
    }

    /** Copy a consistent snapshot out of the seqlock. False = no writer yet. */
    bool read(anland_xr_pose* out) {
        open_file();
        if (!block) return false;
        for (int tries = 0; tries < 8; tries++) {
            uint32_t s0 = anland_xr_seq_read(&block->seq);
            if (s0 & 1u) continue;
            anland_xr_pose tmp;
            memcpy(&tmp, &block->pose, sizeof(tmp));
            uint32_t s1 = anland_xr_seq_read(&block->seq);
            if (s0 != s1) continue;
            if (block->magic != ANLAND_XR_POSE_MAGIC) return false;
            *out = tmp;
            return true;
        }
        return false;
    }
};

struct InputFile {
    int fd = -1;
    const anland_xr_input_block* block = nullptr;

    void open_file() {
        if (block) return;
        char path[512];
        if (anland_xr_path(ANLAND_XR_INPUT_FILE, path, sizeof(path)) != 0) return;
        fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        void* p = ::mmap(nullptr, ANLAND_XR_BLOCK_BYTES, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            ::close(fd);
            fd = -1;
            return;
        }
        block = static_cast<const anland_xr_input_block*>(p);
    }

    bool read(anland_xr_input* out) {
        open_file();
        if (!block) return false;
        for (int tries = 0; tries < 8; tries++) {
            uint32_t s0 = anland_xr_seq_read(&block->seq);
            if (s0 & 1u) continue;
            anland_xr_input tmp;
            memcpy(&tmp, &block->input, sizeof(tmp));
            uint32_t s1 = anland_xr_seq_read(&block->seq);
            if (s0 != s1) continue;
            if (block->magic != ANLAND_XR_INPUT_MAGIC) return false;
            *out = tmp;
            return true;
        }
        return false;
    }
};

/** wall-clock shared by everything: Android and Linux both boot off the same
 *  monotonic clock on this device, but the files carry CLOCK_MONOTONIC ns. */
inline uint64_t mono_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** Read the daemon's display description (rewritten on each XR config change). */
bool read_display(anland_xr_display* out) {
    char path[512];
    if (anland_xr_path(ANLAND_XR_DISPLAY_FILE, path, sizeof(path)) != 0) return false;
    FILE* f = ::fopen(path, "r");
    if (!f) return false;
    char buf[2048];
    size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::fclose(f);
    buf[n] = 0;
    return anland_xr_display_parse(buf, out) == 0;
}

/* small quaternion helpers, HC layout [x,y,z,w] */
inline void qmul(const double a[4], const double b[4], double out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

inline void qnormalize(double q[4]) {
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-9) { q[0] = q[1] = q[2] = 0.0; q[3] = 1.0; return; }
    q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n;
}

/** Rotate a vector by a quaternion. */
inline void qrotate(const double q[4], const double v[3], double out[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2.0 * (y * v[2] - z * v[1]);
    const double ty = 2.0 * (z * v[0] - x * v[2]);
    const double tz = 2.0 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/** Shared snapshot of everything the daemon publishes. */
struct Tracking {
    std::mutex lock;
    anland_xr_pose pose{};
    anland_xr_input input{};
    anland_xr_display display{};
    bool have_pose = false;
    bool have_input = false;
    uint64_t pose_age_ns = ~0ull;
    /* readers are mmap handles that stay open for the driver's lifetime */
    PoseFile pose_file;
    InputFile input_file;

    void pump() {
        std::lock_guard<std::mutex> lk(lock);
        anland_xr_pose p;
        if (pose_file.read(&p)) {
            pose = p;
            have_pose = true;
            const uint64_t now = mono_ns();
            pose_age_ns = (p.sample_ns && now > p.sample_ns) ? now - p.sample_ns : 0;
        }
        anland_xr_input in;
        if (input_file.read(&in)) {
            input = in;
            have_input = true;
        }
    }

    bool pose_fresh() {
        std::lock_guard<std::mutex> lk(lock);
        return have_pose && pose_age_ns < 250ull * 1000ull * 1000ull;
    }
};

Tracking g_tracking;

void log_(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (IVRDriverLog* l = VRDriverLog()) l->Log((std::string(kLogTag) + buf).c_str());
} 

}  // namespace

/* ------------------------------------------------------------------------- */
/* HMD                                                                        */
/* ------------------------------------------------------------------------- */

class AnlandHmd : public ITrackedDeviceServerDriver, public IVRDisplayComponent {
public:
    AnlandHmd() = default;
    virtual ~AnlandHmd() = default;

    EVRInitError Activate(uint32_t unObjectId) override {
        object_id_ = unObjectId;
        const PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer((TrackedDeviceIndex_t)unObjectId);
        const anland_xr_display& d = g_tracking.display;

        VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "anland");
        VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "anland xr");
        VRProperties()->SetStringProperty(c, Prop_SerialNumber_String, "anland-hmd");
        VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "anland");
        VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, "generic_hmd");
        VRProperties()->SetStringProperty(c, Prop_RegisteredDeviceType_String, "anland/hmd");
        VRProperties()->SetInt32Property(c, Prop_DeviceClass_Int32, (int32_t)TrackedDeviceClass_HMD);

        VRProperties()->SetFloatProperty(c, Prop_DisplayFrequency_Float, (float)d.refresh_hz);
        VRProperties()->SetFloatProperty(c, Prop_UserIpdMeters_Float, (float)d.ipd);
        VRProperties()->SetFloatProperty(c, Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
        VRProperties()->SetFloatProperty(c, Prop_FieldOfViewLeftDegrees_Float,
                                         (float)(atan(d.fov_left) * 180.0 / M_PI));
        VRProperties()->SetFloatProperty(c, Prop_FieldOfViewRightDegrees_Float,
                                         (float)(atan(d.fov_right) * 180.0 / M_PI));
        VRProperties()->SetFloatProperty(c, Prop_FieldOfViewTopDegrees_Float,
                                         (float)(atan(d.fov_top) * 180.0 / M_PI));
        VRProperties()->SetFloatProperty(c, Prop_FieldOfViewBottomDegrees_Float,
                                         (float)(atan(d.fov_bottom) * 180.0 / M_PI));
        /* the panel is a phone / AR glasses screen: no hardware proximity
         * sensor, and telling SteamVR otherwise parks the HMD in standby the
         * moment the user holds still */
        VRProperties()->SetBoolProperty(c, Prop_ContainsProximitySensor_Bool, false);
        VRProperties()->SetBoolProperty(c, Prop_WillDriftInYaw_Bool, false);
        VRProperties()->SetBoolProperty(c, Prop_HasDriverDirectModeComponent_Bool, false);
        VRProperties()->SetBoolProperty(c, Prop_HasCameraComponent_Bool, false);
        VRProperties()->SetBoolProperty(c, Prop_HasDisplayComponent_Bool, true);
        /* windowed output: the daemon composites vrcompositor's own window, so
         * the compositor must NOT try to take over a display */
        VRProperties()->SetBoolProperty(c, Prop_HasVirtualDisplayComponent_Bool, false);
        VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32,
                                         (int32_t)TrackedControllerRole_Invalid);

        log_("HMD activated: id=%u %ux%u/eye @%.1f Hz mode=%u ipd=%.1fmm",
             unObjectId, d.eye_width, d.eye_height, d.refresh_hz, d.mode, d.ipd * 1000.0);
        active_ = true;
        return VRInitError_None;
    }

    void Deactivate() override { active_ = false; }
    void EnterStandby() override { }
    void* GetComponent(const char* nameAndVersion) override {
        if (nameAndVersion && strcmp(nameAndVersion, IVRDisplayComponent_Version) == 0)
            return static_cast<IVRDisplayComponent*>(this);
        return nullptr;
    }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (response && size) snprintf(response, size, "anland hmd: %s",
                                       g_tracking.pose_fresh() ? "tracking" : "no pose");
    }

    DriverPose_t GetPose() override {
        DriverPose_t pose;
        memset(&pose, 0, sizeof(pose));
        pose.qWorldFromDriverRotation.w = 1.0;
        pose.qDriverFromHeadRotation.w = 1.0;
        pose.poseIsValid = false;
        pose.deviceIsConnected = true;
        pose.result = TrackingResult_Calibrating_InProgress;
        pose.shouldApplyHeadModel = false;
        pose.willDriftInYaw = false;

        std::lock_guard<std::mutex> lk(g_tracking.lock);
        if (!g_tracking.have_pose) return pose;
        const anland_xr_pose& p = g_tracking.pose;
        pose.poseIsValid = p.tracking != 0;
        pose.result = pose.poseIsValid ? TrackingResult_Running_OK
                                       : TrackingResult_Calibrating_InProgress;
        pose.qRotation.x = p.quat[0];
        pose.qRotation.y = p.quat[1];
        pose.qRotation.z = p.quat[2];
        pose.qRotation.w = p.quat[3];
        pose.vecPosition[0] = p.pos[0];
        pose.vecPosition[1] = p.pos[1];
        pose.vecPosition[2] = p.pos[2];
        /* Angular velocity lets the compositor extrapolate between samples
         * (and SteamVR's own prediction uses it). Body frame ≈ tracking frame
         * for small rates, which is all this is good for. */
        pose.vecAngularVelocity[0] = p.ang_vel[0];
        pose.vecAngularVelocity[1] = p.ang_vel[1];
        pose.vecAngularVelocity[2] = p.ang_vel[2];
        /* the pose is already in the daemon's frame; sample_ns lets the
         * compositor extrapolate if it is late by a few ms */
        pose.poseTimeOffset = 0.0;
        return pose;
    }

    /* ---- IVRDisplayComponent ---- */

    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override {
        const anland_xr_display& d = g_tracking.display;
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = d.mode == ANLAND_XR_MODE_SBS_H ? d.eye_width * 2 : d.eye_width;
        if (h) *h = d.mode == ANLAND_XR_MODE_SBS_V ? d.eye_height * 2 : d.eye_height;
    }

    /** true: this is a normal window on the desktop (the daemon composites it —     *  anland never takes the display over from SurfaceFlinger). */
    bool IsDisplayOnDesktop() override { return true; }
    /** false: not a physical HMD panel, so the compositor keeps the windowed
     *  output path and never tries VK_KHR_display direct mode. */
    bool IsDisplayRealDisplay() override { return false; }

    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        const anland_xr_display& d = g_tracking.display;
        if (w) *w = d.eye_width;
        if (h) *h = d.eye_height;
    }

    void GetEyeOutputViewport(EVREye eye, uint32_t* x, uint32_t* y,
                              uint32_t* w, uint32_t* h) override {
        const anland_xr_display& d = g_tracking.display;
        if (x) *x = (d.mode == ANLAND_XR_MODE_SBS_H && eye == Eye_Right) ? d.eye_width : 0;
        if (y) *y = (d.mode == ANLAND_XR_MODE_SBS_V && eye == Eye_Right) ? d.eye_height : 0;
        if (w) *w = d.eye_width;
        if (h) *h = d.eye_height;
    }

    void GetProjectionRaw(EVREye, float* left, float* right, float* top,
                          float* bottom) override {
        const anland_xr_display& d = g_tracking.display;
        if (left) *left = (float)-d.fov_left;
        if (right) *right = (float)d.fov_right;
        if (top) *top = (float)-d.fov_top;     /* OpenVR: top is negative */
        if (bottom) *bottom = (float)d.fov_bottom;
    }

    /** Identity: the daemon applies the lens distortion itself (that is the
     *  whole point of XR window compositing), so the compositor must not add
     *  a second warp. */
    DistortionCoordinates_t ComputeDistortion(EVREye, float u, float v) override {
        DistortionCoordinates_t out;
        out.rfRed[0] = out.rfGreen[0] = out.rfBlue[0] = u;
        out.rfRed[1] = out.rfGreen[1] = out.rfBlue[1] = v;
        return out;
    }

    bool ComputeInverseDistortion(HmdVector2_t* result, EVREye, uint32_t,
                                  float u, float v) override {
        if (result) { result->v[0] = u; result->v[1] = v; }
        return true;
    }

    /** tracked-device index this HMD reported to (used by RunFrame) */
    TrackedDeviceIndex_t tracked_index() const { return (TrackedDeviceIndex_t)object_id_; }

private:
    uint32_t object_id_ = k_unTrackedDeviceIndexInvalid;
    bool active_ = false;
};

/* ------------------------------------------------------------------------- */
/* controllers                                                                */
/* ------------------------------------------------------------------------- */

class AnlandController : public ITrackedDeviceServerDriver {
public:
    AnlandController(int slot, const char* serial) : slot_(slot), serial_(serial) {}
    virtual ~AnlandController() = default;

    EVRInitError Activate(uint32_t unObjectId) override {
        object_id_ = unObjectId;
        const PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer((TrackedDeviceIndex_t)unObjectId);
        const bool right = slot_ == 1;

        VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "anland");
        VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "anland controller");
        VRProperties()->SetStringProperty(c, Prop_SerialNumber_String, serial_.c_str());
        VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "anland");
        VRProperties()->SetStringProperty(c, Prop_RenderModelName_String, "vr_controller_vive_1_5");
        VRProperties()->SetStringProperty(c, Prop_RegisteredDeviceType_String, "anland/controller");
        /* vive_controller is the simplest profile with trigger + trackpad +
         * grip + menu; anything the Android pad does not have simply never
         * changes. */
        VRProperties()->SetStringProperty(c, Prop_ControllerType_String, "vive_controller");
        VRProperties()->SetInt32Property(c, Prop_ControllerRoleHint_Int32,
            (int32_t)(right ? TrackedControllerRole_RightHand
                            : TrackedControllerRole_LeftHand));
        VRProperties()->SetInt32Property(c, Prop_DeviceClass_Int32,
                                         (int32_t)TrackedDeviceClass_Controller);
        VRProperties()->SetBoolProperty(c, Prop_DeviceIsWireless_Bool, true);
        VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);
        VRProperties()->SetBoolProperty(c, Prop_DeviceCanPowerOff_Bool, false);
        VRProperties()->SetBoolProperty(c, Prop_ContainsProximitySensor_Bool, false);

        /* components expected by the vive_controller input profile */
        IVRDriverInput* in = VRDriverInput();
        if (in) {
            in->CreateBooleanComponent(c, "/input/trigger/click", &h_trigger_click_);
            in->CreateScalarComponent(c, "/input/trigger/value", &h_trigger_,
                                      VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
            in->CreateBooleanComponent(c, "/input/grip/click", &h_grip_click_);
            in->CreateScalarComponent(c, "/input/grip/value", &h_grip_,
                                      VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
            in->CreateScalarComponent(c, "/input/trackpad/x", &h_pad_x_,
                                      VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
            in->CreateScalarComponent(c, "/input/trackpad/y", &h_pad_y_,
                                      VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
            in->CreateBooleanComponent(c, "/input/trackpad/click", &h_pad_click_);
            in->CreateBooleanComponent(c, "/input/trackpad/touch", &h_pad_touch_);
            in->CreateBooleanComponent(c, "/input/system/click", &h_system_);
            in->CreateBooleanComponent(c, "/input/menu/click", &h_menu_);
            in->CreateBooleanComponent(c, "/input/a/click", &h_a_);
            in->CreateBooleanComponent(c, "/input/b/click", &h_b_);
            in->CreateHapticComponent(c, "/output/haptic", &h_haptic_);
        }
        active_ = true;
        log_("controller slot %d activated: id=%u", slot_, unObjectId);
        return VRInitError_None;
    }

    void Deactivate() override { active_ = false; }
    void EnterStandby() override { }
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (response && size) snprintf(response, size, "anland controller slot %d", slot_);
    }

    DriverPose_t GetPose() override {
        DriverPose_t pose;
        memset(&pose, 0, sizeof(pose));
        pose.qWorldFromDriverRotation.w = 1.0;
        pose.qDriverFromHeadRotation.w = 1.0;
        pose.shouldApplyHeadModel = false;
        pose.willDriftInYaw = false;
        pose.deviceIsConnected = true;
        pose.poseIsValid = false;
        pose.result = TrackingResult_Calibrating_InProgress;

        std::lock_guard<std::mutex> lk(g_tracking.lock);
        if (!g_tracking.have_pose) return pose;
        const anland_xr_pose& p = g_tracking.pose;
        const anland_xr_input& in = g_tracking.input;
        if (slot_ < (int)in.n && in.c[slot_].present) {
            const bool right = slot_ == 1;
            /* A tracked provider (hand tracking bridge, or a future per-hand
             * IMU) may fill the controller's own quat/pos in anland-xr-input;
             * then the pose is real and the offset must not be added. Identity
             * quaternion + zero position is the documented "untracked" value,
             * which keeps the gamepad fallback below. */
            const anland_xr_controller& c = in.c[slot_];
            const double qn = fabs(c.quat[0]) + fabs(c.quat[1]) + fabs(c.quat[2]) +
                              fabs(c.quat[3] - 1.0) + fabs(c.pos[0]) + fabs(c.pos[1]) +
                              fabs(c.pos[2]);
            if (qn > 1e-6) {
                pose.qRotation.x = c.quat[0];
                pose.qRotation.y = c.quat[1];
                pose.qRotation.z = c.quat[2];
                pose.qRotation.w = c.quat[3];
                for (int i = 0; i < 3; i++) pose.vecPosition[i] = c.pos[i];
                pose.poseIsValid = true;
                pose.result = TrackingResult_Running_OK;
                return pose;
            }
            /* untracked: the pad is held in this hand — head pose + fixed
             * shoulder offset, rotated into the head frame so it follows the
             * gaze */
            const double offset[3] = { right ? 0.18 : -0.18, -0.22, -0.32 };
            double rotated[3];
            qrotate(p.quat, offset, rotated);
            pose.qRotation.x = p.quat[0];
            pose.qRotation.y = p.quat[1];
            pose.qRotation.z = p.quat[2];
            pose.qRotation.w = p.quat[3];
            pose.vecPosition[0] = p.pos[0] + rotated[0];
            pose.vecPosition[1] = p.pos[1] + rotated[1];
            pose.vecPosition[2] = p.pos[2] + rotated[2];
            pose.poseIsValid = true;
            pose.result = TrackingResult_Running_OK;
        }
        return pose;
    }

    /** Push the Android pad state into the OpenVR components (RunFrame). */
    void pumpInput(const anland_xr_input& in) {
        IVRDriverInput* d = VRDriverInput();
        if (!d || !active_) return;
        const anland_xr_controller idle{};
        const anland_xr_controller& c =
            (slot_ < (int)in.n) ? in.c[slot_] : idle;
        const bool present = c.present != 0;
        const uint32_t b = present ? c.buttons : 0;

        if (h_trigger_) d->UpdateScalarComponent(h_trigger_, present ? c.trigger : 0.f, 0.0);
        if (h_trigger_click_)
            d->UpdateBooleanComponent(h_trigger_click_, present && (b & ANLAND_XR_BTN_TRIGGER_CLICK) != 0, 0.0);
        if (h_grip_) d->UpdateScalarComponent(h_grip_, present ? c.grip : 0.f, 0.0);
        if (h_grip_click_)
            d->UpdateBooleanComponent(h_grip_click_, present && (b & ANLAND_XR_BTN_GRIP_CLICK) != 0, 0.0);
        if (h_pad_x_) d->UpdateScalarComponent(h_pad_x_, present ? c.trackpad_x : 0.f, 0.0);
        if (h_pad_y_) d->UpdateScalarComponent(h_pad_y_, present ? c.trackpad_y : 0.f, 0.0);
        if (h_pad_click_)
            d->UpdateBooleanComponent(h_pad_click_, present && (b & ANLAND_XR_BTN_STICK_CLICK) != 0, 0.0);
        if (h_pad_touch_)
            d->UpdateBooleanComponent(h_pad_touch_, present && c.pad_pressed > 0.5f, 0.0);
        if (h_system_) d->UpdateBooleanComponent(h_system_, present && (b & ANLAND_XR_BTN_SYSTEM) != 0, 0.0);
        if (h_menu_) d->UpdateBooleanComponent(h_menu_, present && (b & ANLAND_XR_BTN_MENU) != 0, 0.0);
        if (h_a_) d->UpdateBooleanComponent(h_a_, present && (b & ANLAND_XR_BTN_A) != 0, 0.0);
        if (h_b_) d->UpdateBooleanComponent(h_b_, present && (b & ANLAND_XR_BTN_B) != 0, 0.0);
    }

    bool active() const { return active_; }
    TrackedDeviceIndex_t tracked_index() const { return (TrackedDeviceIndex_t)object_id_; }

private:
    int slot_;
    std::string serial_;
    uint32_t object_id_ = k_unTrackedDeviceIndexInvalid;
    bool active_ = false;

    VRInputComponentHandle_t h_trigger_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_trigger_click_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_grip_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_grip_click_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_pad_x_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_pad_y_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_pad_click_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_pad_touch_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_system_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_menu_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_a_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_b_ = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t h_haptic_ = k_ulInvalidInputComponentHandle;
};

/* ------------------------------------------------------------------------- */
/* provider                                                                   */
/* ------------------------------------------------------------------------- */

class AnlandProvider : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext* ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        log_("init: runtime dir %s", anland_xr_runtime_dir());

        /* geometry comes from the daemon; retry briefly —the daemon writes it
         * at startup, and SteamVR may win the race after a reboot */
        for (int i = 0; i < 50; i++) {
            if (read_display(&g_tracking.display)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (g_tracking.display.eye_width == 0)
            anland_xr_display_defaults(&g_tracking.display);
        log_("display: %ux%u/eye mode=%u %.1f Hz ipd=%.1fmm",
             g_tracking.display.eye_width, g_tracking.display.eye_height,
             g_tracking.display.mode, g_tracking.display.refresh_hz,
             g_tracking.display.ipd * 1000.0);

        g_tracking.pump();

        hmd_ = new AnlandHmd();
        if (!VRServerDriverHost()->TrackedDeviceAdded("anland-hmd",
                TrackedDeviceClass_HMD, hmd_)) {
            log_("TrackedDeviceAdded(hmd) failed");
            return VRInitError_Driver_Failed;
        }
        left_ = new AnlandController(0, "anland-ctrl-left");
        right_ = new AnlandController(1, "anland-ctrl-right");
        if (!VRServerDriverHost()->TrackedDeviceAdded("anland-ctrl-left",
                TrackedDeviceClass_Controller, left_))
            log_("TrackedDeviceAdded(left) failed");
        if (!VRServerDriverHost()->TrackedDeviceAdded("anland-ctrl-right",
                TrackedDeviceClass_Controller, right_))
            log_("TrackedDeviceAdded(right) failed");

        display_poll_ = 0;
        return VRInitError_None;
    }

    void Cleanup() override {
        log_("cleanup");
        delete hmd_; hmd_ = nullptr;
        delete left_; left_ = nullptr;
        delete right_; right_ = nullptr;
    }

    const char* const* GetInterfaceVersions() override {
        static const char* const versions[] = { IServerTrackedDeviceProvider_Version, nullptr };
        return versions;
    }

    void RunFrame() override {
        g_tracking.pump();
        /* poses: push only when we have data (a stale HMD reads as "not
         * tracking", which parks games in their loading screen) */
        if (hmd_ && g_tracking.have_pose) {
            DriverPose_t p = hmd_->GetPose();
            VRServerDriverHost()->TrackedDevicePoseUpdated(hmd_->tracked_index(),
                p, sizeof(DriverPose_t));
        }
        if (left_ && left_->active()) {
            DriverPose_t p = left_->GetPose();
            VRServerDriverHost()->TrackedDevicePoseUpdated(left_->tracked_index(),
                p, sizeof(DriverPose_t));
        }
        if (right_ && right_->active()) {
            DriverPose_t p = right_->GetPose();
            VRServerDriverHost()->TrackedDevicePoseUpdated(right_->tracked_index(),
                p, sizeof(DriverPose_t));
        }
        if (g_tracking.have_input) {
            anland_xr_input in;
            {
                std::lock_guard<std::mutex> lk(g_tracking.lock);
                in = g_tracking.input;
            }
            if (left_) left_->pumpInput(in);
            if (right_) right_->pumpInput(in);
        }
        /* the daemon rewrites the display file on every XR config change */
        if (++display_poll_ % 60 == 0) {
            anland_xr_display d = g_tracking.display;
            if (read_display(&d)) {
                std::lock_guard<std::mutex> lk(g_tracking.lock);
                g_tracking.display = d;
            }
        }
    }

    bool ShouldBlockStandbyMode() override { return true; }
    void EnterStandby() override { }
    void LeaveStandby() override { }

private:
    AnlandHmd* hmd_ = nullptr;
    AnlandController* left_ = nullptr;
    AnlandController* right_ = nullptr;
    unsigned display_poll_ = 0;
};

static AnlandProvider g_provider;

/* Driver entry point (SteamVR dlopens the .so and calls this). */
extern "C" __attribute__((visibility("default")))
void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (strcmp(interfaceName, IServerTrackedDeviceProvider_Version) == 0) {
        if (returnCode) *returnCode = VRInitError_None;
        return &g_provider;
    }
    if (returnCode) *returnCode = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
