/*
 * AwlXrSession — head tracking + controller state for an XR window.
 *
 * Part of libawl (anland's window-hosting library). An XR window is a window
 * that the daemon marked XR (config xr_match, default "vrcompositor,steamvr"):
 * the daemon composites it pre-distorted and stereo, and needs to know where
 * the head is pointing so it can reproject (timewarp) between the frame the
 * guest rendered and the frame the display shows. The guest needs the same
 * pose for its OpenVR driver (xr/driver), which is why the data travels
 * Android → daemon over binder and daemon → guest through the shared files in
 * /run/anland (see xr/include/anland_xr_wire.h).
 *
 * Deliberately small:
 *   - orientation only (3DoF). No positional tracking: phones and AR glasses
 *     without SLAM cannot provide it, and SteamVR is happy with a 3DoF HMD.
 *     TYPE_GAME_ROTATION_VECTOR is the primary source (drift-free in yaw for
 *     short sessions, no magnetometer noise); when a device lacks it we fall
 *     back to gyro integration corrected by the accelerometer's gravity
 *     direction (a tilt-only complementary filter — still fine for looking
 *     around, drifts slowly in yaw).
 *   - controllers are gamepads/Joy-Cons seen by this Activity: their buttons
 *     and sticks are pushed as OpenVR input, and their pose is the head pose
 *     plus a fixed shoulder offset (they are not independently tracked, and
 *     pretending otherwise would mislead the game).
 *
 * Threading: sensors and the input push loop live on one HandlerThread; the
 * controller state is touched from the UI thread too (dispatchKeyEvent /
 * dispatchGenericMotionEvent), so the small state blocks below are
 * synchronized.
 */
package com.anlandnext.awl;

import android.app.Activity;
import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

final class AwlXrSession implements SensorEventListener {
    private static final String TAG = "anland-xr";

    /** Two tracked controllers (see ANLAND_XR_MAX_CONTROLLERS) */
    private static final int NC = 2;
    /** Slot 0 = left hand, 1 = right hand: the first gamepad we see drives the
     *  right hand, a second one the left (single-pad play keeps the right hand
     *  as the dominant one). */
    private static final int SLOT_LEFT = 0;
    private static final int SLOT_RIGHT = 1;

    /* wire button bits (must match anland_xr_wire.h) */
    private static final int BTN_A = 1 << 0;
    private static final int BTN_B = 1 << 1;
    private static final int BTN_MENU = 1 << 2;
    private static final int BTN_SYSTEM = 1 << 3;
    private static final int BTN_STICK = 1 << 4;
    private static final int BTN_GRIP = 1 << 5;
    private static final int BTN_TRIGGER = 1 << 6;
    private static final int BTN_DPAD_UP = 1 << 7;
    private static final int BTN_DPAD_DOWN = 1 << 8;
    private static final int BTN_DPAD_LEFT = 1 << 9;
    private static final int BTN_DPAD_RIGHT = 1 << 10;

    private final Activity host;
    private final long windowId;
    private final SensorManager sm;

    private HandlerThread thread;
    private Handler handler;
    private boolean running;

    private Sensor rotVec;      /* TYPE_GAME_ROTATION_VECTOR (preferred) */
    private Sensor gyro;
    private Sensor accel;

    /* orientation: device → world, [x,y,z,w] */
    private final float[] q = {0f, 0f, 0f, 1f};
    /** Gravity estimate in device frame (only used by the gyro fallback) */
    private final float[] grav = {0f, 0f, 0f};
    private boolean haveGrav;
    private long lastGyroNs;
    private int periodNs;
    private long lastPoseNs;

    /* rotation rate (rad/s, device frame) for xr_predict: differenced from the
     * last quaternion we pushed, so it matches whatever produced the pose */
    private final float[] w = {0f, 0f, 0f};
    private final float[] qPrevP = {0f, 0f, 0f, 1f};
    private long qPrevNs;

    /* controller state, index = slot */
    private final int[] buttons = new int[NC];
    private final float[] trigger = new float[NC];
    private final float[] grip = new float[NC];
    private final float[] stickX = new float[NC];
    private final float[] stickY = new float[NC];
    private final float[] trackX = new float[NC];
    private final float[] trackY = new float[NC];
    private final float[] padPress = new float[NC];
    private final boolean[] present = new boolean[NC];
    private final int[] devId = {-1, -1};

    private final int[] push = new int[1 + NC * 9];
    private final Runnable pushTick = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            pushInput();
            handler.postDelayed(this, 16);   /* ~60 Hz: the driver interpolates nothing */
        }
    };

    AwlXrSession(Activity host, long windowId) {
        this.host = host;
        this.windowId = windowId;
        this.sm = (SensorManager) host.getSystemService(Context.SENSOR_SERVICE);
    }

    /** Idempotent: register sensors and start the input tick. */
    void start() {
        if (running) return;
        running = true;
        thread = new HandlerThread("anland-xr");
        thread.start();
        handler = new Handler(thread.getLooper());
        if (sm != null) {
            rotVec = sm.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
            gyro = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
            accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            boolean ok;
            if (rotVec != null) {
                ok = sm.registerListener(this, rotVec, SensorManager.SENSOR_DELAY_GAME, handler);
            } else {
                ok = gyro != null && sm.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME, handler);
                if (accel != null)
                    sm.registerListener(this, accel, SensorManager.SENSOR_DELAY_GAME, handler);
            }
            Log.i(TAG, "win " + windowId + ": tracking started ("
                    + (rotVec != null ? "game-rotation-vector" : "gyro+accel fallback")
                    + (ok ? "" : " — no sensor!") + ")");
        } else {
            Log.w(TAG, "win " + windowId + ": no SensorManager, controllers only");
        }
        handler.postDelayed(pushTick, 16);
    }

    /** Idempotent: sensors off, tick stopped. The daemon keeps rendering with
     *  the last pose (it goes stale after 250 ms and it stops reprojecting). */
    void stop() {
        if (!running) return;
        running = false;
        if (handler != null) handler.removeCallbacks(pushTick);
        if (sm != null) sm.unregisterListener(this);
        if (thread != null) {
            thread.quitSafely();
            thread = null;
        }
        handler = null;
        Log.i(TAG, "win " + windowId + ": tracking stopped");
    }

    void shutdown() {
        stop();
        present[0] = present[1] = false;
        pushInput();
    }

    /* ---------------- sensors ---------------- */

    @Override
    public void onSensorChanged(SensorEvent ev) {
        if (!running) return;
        final int type = ev.sensor.getType();
        final long nowNs = ev.timestamp;
        if (type == Sensor.TYPE_GAME_ROTATION_VECTOR) {
            /* already a quaternion (x,y,z,w); w may be absent on some HALs */
            float x = ev.values[0], y = ev.values[1], z = ev.values[2];
            float w = ev.values.length > 3 ? ev.values[3] : 0f;
            if (ev.values.length <= 3) w = (float) Math.sqrt(Math.max(0.0, 1.0 - (x * x + y * y + z * z)));
            q[0] = x; q[1] = y; q[2] = z; q[3] = w;
            qnormalize(q);
        } else if (type == Sensor.TYPE_GYROSCOPE) {
            /* rad/s in device frame; integrate q += ½·q⊗(0,ω)·dt */
            final float dt = lastGyroNs == 0 ? 0.01f : (nowNs - lastGyroNs) * 1e-9f;
            lastGyroNs = nowNs;
            if (dt > 0f && dt < 0.2f) {
                qintegrate(q, ev.values[0], ev.values[1], ev.values[2], dt);
                if (haveGrav) qtiltTowards(q, grav, 0.02f);
            }
        } else if (type == Sensor.TYPE_ACCELEROMETER) {
            grav[0] = ev.values[0];
            grav[1] = ev.values[1];
            grav[2] = ev.values[2];
            haveGrav = true;
        } else {
            return;
        }
        final long dt = nowNs - lastPoseNs;
        if (dt >= 4_000_000L) {   /* cap at 250 Hz: binder ONEWAY has to breathe */
            lastPoseNs = nowNs;
            /* Rotation rate for the daemon's xr_predict: finite difference of the
             * quaternion we are about to send. Doing it here (rather than
             * forwarding the raw gyro) keeps the frames consistent — the pose
             * may come from the fused rotation vector, not the gyro. */
            if (qPrevNs != 0) {
                final float sdt = (nowNs - qPrevNs) * 1e-9f;
                if (sdt > 1e-4f && sdt < 0.2f) qangvel(qPrevP, q, sdt, w);
            }
            System.arraycopy(q, 0, qPrevP, 0, 4);
            qPrevNs = nowNs;
            AwlClient.xrPose(windowId, q[0], q[1], q[2], q[3], 0f, 0f, 0f,
                    w[0], w[1], w[2], nowNs, periodNs);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) { }

    /* ---------------- controllers ---------------- */

    /** @return true when the key belongs to a VR controller and was consumed */
    boolean onKeyEvent(KeyEvent ev) {
        final int src = ev.getSource();
        if ((src & InputDevice.SOURCE_JOYSTICK) == 0
                && (src & InputDevice.SOURCE_GAMEPAD) == 0)
            return false;   /* keyboard: the wayland client wants it */
        final int slot = slotFor(ev.getDeviceId());
        if (slot < 0) return false;
        final boolean down = ev.getAction() == KeyEvent.ACTION_DOWN;
        int bit = 0;
        switch (ev.getKeyCode()) {
        case KeyEvent.KEYCODE_BUTTON_A: bit = BTN_A; break;
        case KeyEvent.KEYCODE_BUTTON_B: bit = BTN_B; break;
        case KeyEvent.KEYCODE_BUTTON_X: bit = BTN_A; break;   /* left hand: X/Y at A/B */
        case KeyEvent.KEYCODE_BUTTON_Y: bit = BTN_B; break;
        case KeyEvent.KEYCODE_BUTTON_START: bit = BTN_MENU; break;
        case KeyEvent.KEYCODE_BUTTON_SELECT: bit = BTN_SYSTEM; break;
        case KeyEvent.KEYCODE_BUTTON_THUMBL:
        case KeyEvent.KEYCODE_BUTTON_THUMBR: bit = BTN_STICK; break;
        case KeyEvent.KEYCODE_BUTTON_L1:
        case KeyEvent.KEYCODE_BUTTON_R1: bit = BTN_GRIP; break;
        case KeyEvent.KEYCODE_BUTTON_L2:
        case KeyEvent.KEYCODE_BUTTON_R2: bit = BTN_TRIGGER; break;
        case KeyEvent.KEYCODE_DPAD_UP: bit = BTN_DPAD_UP; break;
        case KeyEvent.KEYCODE_DPAD_DOWN: bit = BTN_DPAD_DOWN; break;
        case KeyEvent.KEYCODE_DPAD_LEFT: bit = BTN_DPAD_LEFT; break;
        case KeyEvent.KEYCODE_DPAD_RIGHT: bit = BTN_DPAD_RIGHT; break;
        default: return false;
        }
        synchronized (this) {
            if (down) buttons[slot] |= bit;
            else buttons[slot] &= ~bit;
            if (bit == BTN_STICK) padPress[slot] = down ? 1f : 0f;
        }
        return true;
    }

    /** @return true when the motion belongs to a VR controller and was consumed */
    boolean onMotionEvent(MotionEvent ev) {
        final int src = ev.getSource();
        if ((src & InputDevice.SOURCE_JOYSTICK) == 0) return false;
        final int slot = slotFor(ev.getDeviceId());
        if (slot < 0) return false;
        float lx = ev.getAxisValue(MotionEvent.AXIS_X);
        float ly = ev.getAxisValue(MotionEvent.AXIS_Y);
        float lt = ev.getAxisValue(MotionEvent.AXIS_LTRIGGER);
        if (lt == 0f) lt = ev.getAxisValue(MotionEvent.AXIS_BRAKE);
        float rt = ev.getAxisValue(MotionEvent.AXIS_RTRIGGER);
        if (rt == 0f) rt = ev.getAxisValue(MotionEvent.AXIS_GAS);
        float hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y);
        float rx = ev.getAxisValue(MotionEvent.AXIS_RX);
        float ry = ev.getAxisValue(MotionEvent.AXIS_RY);
        synchronized (this) {
            stickX[slot] = lx;
            stickY[slot] = ly;
            if (rx != 0f || ry != 0f) { trackX[slot] = rx; trackY[slot] = ry; }
            trigger[slot] = Math.max(lt, rt);
            grip[slot] = Math.min(lt, rt) > 0f ? Math.min(lt, rt) : 0f;
            if (hx != 0f || hy != 0f) {
                buttons[slot] &= ~(BTN_DPAD_UP | BTN_DPAD_DOWN | BTN_DPAD_LEFT | BTN_DPAD_RIGHT);
                if (hy < -0.5f) buttons[slot] |= BTN_DPAD_UP;
                if (hy > 0.5f) buttons[slot] |= BTN_DPAD_DOWN;
                if (hx < -0.5f) buttons[slot] |= BTN_DPAD_LEFT;
                if (hx > 0.5f) buttons[slot] |= BTN_DPAD_RIGHT;
            }
        }
        return true;
    }

    /** Map an Android input device id onto a controller slot (first seen wins
     *  the right hand, the second the left). */
    private int slotFor(int id) {
        synchronized (this) {
            for (int i = 0; i < NC; i++) if (devId[i] == id) return i;
            int slot = devId[SLOT_RIGHT] == -1 ? SLOT_RIGHT
                     : devId[SLOT_LEFT] == -1 ? SLOT_LEFT : -1;
            if (slot < 0) return -1;   /* >2 pads: ignore the extras */
            devId[slot] = id;
            present[slot] = true;
            Log.i(TAG, "controller device " + id + " → slot " + slot
                    + (slot == SLOT_RIGHT ? " (right)" : " (left)"));
            return slot;
        }
    }

    /** Pack and push the controller snapshot (see awl_xr_set_input). */
    private void pushInput() {
        int n = 0;
        synchronized (this) {
            for (int i = 0; i < NC; i++) {
                int o = 1 + i * 9;
                push[o + 0] = present[i] ? 1 : 0;
                push[o + 1] = buttons[i];
                push[o + 2] = fix(trigger[i]);
                push[o + 3] = fix(grip[i]);
                push[o + 4] = fix(stickX[i]);
                push[o + 5] = fix(stickY[i]);
                push[o + 6] = fix(trackX[i]);
                push[o + 7] = fix(trackY[i]);
                push[o + 8] = fix(padPress[i]);
            }
            n = NC * 9;
        }
        AwlClient.xrInput(windowId, push, n);
    }

    private static int fix(float v) {
        if (Float.isNaN(v) || Float.isInfinite(v)) v = 0f;
        if (v > 1f) v = 1f;
        if (v < -1f) v = -1f;
        return Math.round(v * 10000f);
    }

    /* ---------------- quaternion helpers (HC: [x,y,z,w]) ---------------- */

    private static void qnormalize(float[] a) {
        float n = (float) Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
        if (n < 1e-6f) { a[0] = 0; a[1] = 0; a[2] = 0; a[3] = 1; return; }
        a[0] /= n; a[1] /= n; a[2] /= n; a[3] /= n;
    }

    /** q ← q ⊗ exp(½·ω·dt) — body-frame angular velocity integration */
    private static void qintegrate(float[] a, float wx, float wy, float wz, float dt) {
        final float hx = wx * 0.5f * dt, hy = wy * 0.5f * dt, hz = wz * 0.5f * dt;
        /* exact exponential of a pure quaternion (small-angle stable) */
        final float ang = (float) Math.sqrt(hx * hx + hy * hy + hz * hz);
        final float s = ang > 1e-8f ? (float) Math.sin(ang) / ang : 1f;
        final float dw = (float) Math.cos(ang);
        final float dx = hx * s, dy = hy * s, dz = hz * s;
        final float x = a[0], y = a[1], z = a[2], w = a[3];
        a[0] = w * dx + x * dw + y * dz - z * dy;
        a[1] = w * dy - x * dz + y * dw + z * dx;
        a[2] = w * dz + x * dy - y * dx + z * dw;
        a[3] = w * dw - x * dx - y * dy - z * dz;
        qnormalize(a);
    }

    /** ω (rad/s, body frame) from two orientations over dt:
     *  dq = conj(q0) ⊗ q1 = exp(½·ω·dt). Leaves out untouched when the two
     *  orientations are (nearly) equal — the rate is then simply unknown. */
    private static void qangvel(float[] q0, float[] q1, float dt, float[] out) {
        if (dt <= 1e-6f) return;
        /* dq = conj(q0) ⊗ q1, xyz first */
        final float ax = -q0[0], ay = -q0[1], az = -q0[2], aw = q0[3];
        float dx = aw * q1[0] + ax * q1[3] + ay * q1[2] - az * q1[1];
        float dy = aw * q1[1] - ax * q1[2] + ay * q1[3] + az * q1[0];
        float dz = aw * q1[2] + ax * q1[1] - ay * q1[0] + az * q1[3];
        float dw = aw * q1[3] - ax * q1[0] - ay * q1[1] - az * q1[2];
        if (dw > 1f) dw = 1f;
        if (dw < -1f) dw = -1f;
        final float ang = 2f * (float) Math.acos(dw);
        final float sn = (float) Math.sqrt(Math.max(0f, 1f - dw * dw));
        if (sn < 1e-6f) return;   /* no rotation, or an exact half turn */
        final float k = ang / (sn * dt);
        out[0] = dx * k;
        out[1] = dy * k;
        out[2] = dz * k;
    }

    /** Nudge q so that its predicted "up" (device frame) aligns with the
     *  measured gravity direction — tilt-only complementary correction. */
    private static void qtiltTowards(float[] a, float[] g, float gain) {
        final float gl = (float) Math.sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (gl < 1e-3f) return;
        final float ux = -g[0] / gl, uy = -g[1] / gl, uz = -g[2] / gl;   /* measured up */
        /* predicted up = R^T·(0,1,0) */
        final float px = 2f * (a[0] * a[1] - a[3] * a[2]);
        final float py = 1f - 2f * (a[0] * a[0] + a[2] * a[2]);
        final float pz = 2f * (a[1] * a[2] + a[3] * a[0]);
        /* rotation that takes predicted → measured: axis = p×u, angle ≈ |p×u| */
        final float cx = py * uz - pz * uy;
        final float cy = pz * ux - px * uz;
        final float cz = px * uy - py * ux;
        final float s = (float) Math.sqrt(cx * cx + cy * cy + cz * cz);
        if (s < 1e-6f) return;
        final float ang = (float) Math.asin(Math.min(1f, s)) * gain;
        /* q ← q ⊗ q_axis(ang): half-angle form of the correction rotation */
        final float hs = (float) Math.sin(ang * 0.5f), hc = (float) Math.cos(ang * 0.5f);
        final float dx = cx / s * hs, dy = cy / s * hs, dz = cz / s * hs, dw = hc;
        final float x = a[0], y = a[1], z = a[2], w = a[3];
        a[0] = w * dx + x * dw + y * dz - z * dy;
        a[1] = w * dy - x * dz + y * dw + z * dx;
        a[2] = w * dz + x * dy - y * dx + z * dw;
        a[3] = w * dw - x * dx - y * dy - z * dz;
        qnormalize(a);
    }
}
