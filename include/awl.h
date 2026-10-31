/*
 * awl.h — AndroidWayland logic-layer public API (v2, window-driven)
 *
 * Logic layer: the Wayland protocol state machines (compositor/xdg_shell/
 * shm/dmabuf/output). Each xdg_toplevel maps to one Android window
 * (window_id); rendering is done by the adaptation layer (GPU) — the logic
 * layer never touches pixels.
 */
#ifndef AWL_H
#define AWL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

#define AWL_ABI_VERSION 3

/* ---- Window callbacks (implemented by the adaptation layer, invoked on the
 *      client's protocol dispatch thread: the main event thread or its
 *      dedicated sub-event thread; the adaptation layer must provide its own
 *      thread safety) ---- */
typedef struct awl_window_callbacks {
    void* user;

    /* xdg_surface first-frame map → create the Android-side window
     * (Activity). pref_w/h = first-frame buffer size; is_popup: popup-type
     * (menus etc.) */
    void (*window_created)(void* user, uint64_t id, int32_t pref_w, int32_t pref_h,
                           const char* title, int is_popup);
    void (*window_destroyed)(void* user, uint64_t id);
    void (*window_title)(void* user, uint64_t id, const char* title);

    /* Toplevel icon applied/reset (xdg-toplevel-icon-v1, awl_icon.c): the
     * double-buffered set_icon state took effect at the toplevel's surface
     * commit (a reset notifies too — the fetch then returns "none"). Pure
     * state sync on the client's dispatch thread → the adaptation layer
     * tells the Activity over C_ICON; the pixels are pulled per change with
     * awl_window_get_icon. */
    void (*window_icon)(void* user, uint64_t id);

    /* Pointer constraint state change (zwp_pointer_constraints_v1
     * lock/confine request, set_region on a live constraint, or object /
     * surface destruction — pure state sync, always on the client's
     * dispatch thread; the input forwarding path never branches on this) →
     * the adaptation layer tells the Activity over C_CAPTURE: mode =
     * CONFINE/LOCK → requestPointerCapture (captured motion arrives as
     * AWL_IN_PTR_REL, the Activity synthesizes any clamped absolute motion
     * itself), NONE → releasePointerCapture. rx,ry,rw,rh = confine region
     * in Activity view pixels (zeros = whole window for CONFINE, ignored
     * for NONE/LOCK). */
    void (*pointer_lock)(void* user, uint64_t id, int mode,
                         int32_t rx, int32_t ry, int32_t rw, int32_t rh);

    /* surface commit (new buffer ready) → the adaptation layer pulls and renders */
    void (*window_dirty)(void* user, uint64_t id);

    /* ---- IME bridge (text-input protocol ↔ Android IME, see awl_ime.c) ----
     * Passthrough model: everything carries its window id, no routing state;
     * invoked on the client's protocol dispatch thread. */

    /* Client text field gained/lost focus (v1 activate / v3 enable+commit ↔
     * kbd focus) → notify the matching Activity to show/hide the soft
     * keyboard. hint/purpose = zwp_text_input content type (the Activity
     * maps it to InputType; keyboard panel selection for segmentation /
     * prediction). */
    void (*ime_show)(void* user, uint64_t id, uint32_t hint, uint32_t purpose);
    void (*ime_hide)(void* user, uint64_t id);

    /* Client-reported editor state (set_surrounding_text/set_cursor_rectangle
     * take effect with commit_state / v3 commit) → the Activity caches it to
     * answer IME queries (getTextBeforeCursor/getCursorCapsMode/... =
     * segmentation/prediction context) + updateSelection/
     * updateCursorAnchorInfo drive the candidate window to follow the
     * cursor. text = UTF-8 surrounding; cursor/anchor = byte offsets into
     * text; flags: see AWL_IME_STATE_* (e.g. v1 reset → Activity
     * restartInput). */
    void (*ime_state)(void* user, uint64_t id, const char* text,
                      int32_t cursor, int32_t anchor,
                      uint32_t hint, uint32_t purpose,
                      int32_t cx, int32_t cy, int32_t cw, int32_t ch,
                      uint32_t flags);

    /* ---- Clipboard bridge (wl_data_device set_selection → Android
     * clipboard) ----
     * Text already fetched from the source client's pipe (dedicated thread,
     * callback on that thread; ≤256KB UTF-8). win_id = the owning toplevel
     * of the source client (0 = none/clear); an empty utf8 string clears
     * the clipboard. The adaptation layer has the Activity write to
     * ClipboardManager over the ctrl channel (background reads/writes are
     * restricted on Android 10+, the foreground Activity does it on behalf). */
    void (*clipboard_text)(void* user, uint64_t win_id, const char* utf8);

    /* ---- Cursor (wl_pointer.set_cursor, semantics of kwin-6.6.5
     *      PointerInterface::pointer_set_cursor + CursorImage) ----
     * hidden=1: the pointer-focused client took over the cursor for that
     * window (set_cursor with a cursor surface, or NULL = invisible pointer)
     * → the adaptation layer must hide the Android system pointer for that
     * window (View.setPointerIcon(TYPE_NULL)); the cursor image itself is
     * composited by the renderer as the topmost layer of the window
     * (awl_pointer_cursor_layer, positioned from the pointer position the
     * Activity reports with every motion). hidden=0: pointer left the
     * window / (re)entered a window / focus layer or cursor surface
     * destroyed / client gone → restore the system pointer. Only state
     * transitions are reported. Invoked on the client's protocol dispatch
     * thread (set_cursor, surface death) or the input thread (leave/enter). */
    void (*pointer_cursor)(void* user, uint64_t id, int hidden);

    /* ---- Idle inhibitor (zwp_idle_inhibit_manager_v1, awl_idle.c) ----
     * Aggregate inhibitor state of the window flipped (first inhibitor
     * created on one of its surfaces ↔ last one gone, incl. surface
     * death) — pure state sync on the client's dispatch thread, only
     * transitions are reported → the adaptation layer tells the Activity
     * over C_KEEPON to set/clear FLAG_KEEP_SCREEN_ON. The window flag's
     * native Android semantics (honored only while the window is visible)
     * already matches the protocol's "inhibitor honored on a visible
     * surface" requirement, so no daemon-side visibility state exists. */
    void (*idle_inhibit)(void* user, uint64_t id, int on);
} awl_window_callbacks_t;

typedef struct awl_display_info {
    uint32_t width;        /* wl_output logical size */
    uint32_t height;
    int32_t  refresh_hz;
    int32_t  dpi;
    int32_t  scale;        /* wl_output scale factor (1) */
} awl_display_info_t;

/*
 * Start the server (listen_fd already bound+listening; -1 = no accept
 * source — pure binder-fd mode, clients arrive via awl_server_add_client
 * only). Returns 0 on success. The event loop owns a dedicated thread.
 */
int  awl_server_start(int listen_fd,
                      const awl_display_info_t* info,
                      const awl_window_callbacks_t* cbs);
/* Binder-injected client (#36): hand one end of a caller-created socketpair
 * to the server (called on a binder thread; the fd is marshalled onto the
 * main event thread, which alone may run wl_client_create). SO_PEERCRED on
 * that end is fixed at creation time to the CREATOR's credentials — the
 * socketpair must be created by the wayland client app itself, never by
 * this process (a daemon-created pair would stamp every client with the
 * daemon's uid and collapse the window-ownership model). The app side uses
 * wl_display_connect_to_fd on the other end. Returns 0 = handed off (the
 * server owns the fd from here, whatever the outcome), <0 = refused (the
 * caller keeps the fd and closes it). Any thread. */
int  awl_server_add_client(int fd);
void awl_server_stop(void);
int  awl_server_is_running(void);

/* ---- Android → logic layer ----
 * Direct send from any thread (libwayland carries the awl patches:
 * connection mutex + atomic serial): resolve→protocol send→flush all happen
 * inside the binder thread, bypassing the event thread. */
void awl_window_resize(uint64_t id, int32_t w, int32_t h);   /* xdg configure */
/* An Android window of w×h physical px attached/resized: grow the wl_output
 * mode to cover it (per-axis max, never shrinks; no-op when it already fits)
 * and re-announce it to every bound wl_output. Rootless Xwayland sizes its X
 * screen from the output and clamps pointer/touch to it, so an X toplevel
 * larger than the output had unreachable regions. Any thread. */
void awl_output_grow(uint32_t w, uint32_t h);
/* Android window resized → the renderer's cached ANativeWindow size for this
 * window is stale: drop it, the next frame re-queries and renders at the new
 * size (the original full-screen path). Called from awl_window_resize; any
 * thread; unknown window = no-op. */
void awl_renderer_window_resized(uint64_t id);
void awl_window_close(uint64_t id);                          /* xdg close */
int  awl_xwayland_window_serial(uint64_t id, uint64_t* serial);  /* Xwayland
    * window association serial (WL_SURFACE_SERIAL; 1 = an Xwayland window
    * that has been associated, 0 = not) — used to pair X-side operations
    * (resize/close) over the mini-wm control channel */
void awl_window_set_activated(uint64_t id, int activated);   /* ACTIVATED state */
/* Desktop/session window (docs/desktop.md): a nested compositor's toplevel is
 * the whole screen, so every configure sent to it also carries
 * FULLSCREEN|MAXIMIZED. Without that a compositor keeps treating the host as
 * a floating window manager and re-negotiates its own output geometry on
 * every resize. Idempotent and independent of the XR window flag. */
void awl_window_set_session(uint64_t id, int on);
int  awl_window_is_session(uint64_t id);
pid_t awl_window_client_pid(uint64_t id);   /* window id → client host pid, read
    * fresh from the connect-time cached credentials (0 = unknown/destroyed) */
uid_t awl_window_client_uid(uint64_t id);   /* window id → wayland client uid
    * ((uid_t)-1 = unknown/destroyed) — binder SURFACE auth pass compares it
    * against the attaching app's binder uid */

/* window id → current toplevel icon (xdg-toplevel-icon-v1, awl_icon.c):
 * best available buffer (largest width×scale), swizzled to RGBA bytes
 * (malloc'd, caller frees). w/h = pixel dims. 0 = the window has no icon. */
int awl_window_get_icon(uint64_t id, void** pixels, int32_t* w, int32_t* h);
/* Foreground scheduling (awl_sched.c): on = move pid's whole /proc subtree
 * into Android's top-app cgroups, off = back to the root groups. Stateless
 * and synchronous — the adapter calls it on window attach/detach and with
 * getpid() for its own boost. Any thread, no daemon locks held. */
void awl_sched_set(pid_t pid, int on);
void awl_display_set_zoom(int pct);   /* zoom = 100×Z (50..300; dynamic, #31) */
int awl_display_zoom(void);           /* current zoom pct (daemon config reads) */
double awl_zoom_scale(void);          /* effective quantized Z used by clients */

/* View mapping mode (#34, daemon config scale_mode): how the content-base
 * rectangle is placed inside the Android window. Presentation-layer only —
 * configure sizes are unaffected (a client that fills the window's aspect
 * renders 1:1 in every mode; one that keeps a fixed size gets letterboxed
 * instead of stretched). */
enum {
    AWL_SCALE_STRETCH = 0,   /* fill each axis independently (legacy behavior) */
    AWL_SCALE_FIT = 1,       /* uniform scale to fit inside, centered, letterbox */
    AWL_SCALE_CENTER = 2,    /* 1:1, centered (content larger than the window is cropped) */
};

/* scale_mode placement math, view = logical × s + o, for a root whose
 * content does NOT follow the configured size (fixed-size client ignoring
 * resize, X window the X side did not resize, the frames between a
 * configure and its ack). pw/ph = window view px, cw/ch = content base
 * (logical px), rx/ry = buffer px per logical px of the root's buffer:
 *   STRETCH  fill each axis
 *   FIT      uniform scale to fit inside, centered
 *   CENTER   original size: 1 buffer px = 1 view px (s = rx/ry), centered,
 *            never resampled
 * Content that follows the configure never comes here — it is drawn at
 * exactly view = logical × Z (awl_surface_view_map, the per-root decision).
 * Degenerate input (pw/ph ≤ 0 — no resize recorded yet — or cw/ch ≤ 0.5)
 * yields the identity map; an unknown mode falls back to stretch. Pure math,
 * any thread, no locks. */
void awl_view_map(int mode, double pw, double ph, double cw, double ch,
                  double rx, double ry,
                  double* sx, double* sy, double* ox, double* oy);
void awl_display_set_scale_mode(int mode);   /* dynamic; invalid → ignored + LOGE */
int awl_display_scale_mode(void);            /* current mode (daemon config reads) */
/* Initial-configure placeholder size (#33, daemon config init_w/init_h — the
 * size sent before the Android window exists; the real size follows via
 * awl_window_resize once the Activity surface is ready). Applies to new
 * windows only. Any thread. */
void awl_display_set_init_size(int32_t w, int32_t h);
void awl_display_init_size(int32_t* w, int32_t* h);   /* current value (config reads) */

/* ---- Input (Activity → binder → straight to the client, bypassing the
 *      event thread) ----
 * Each event carries its target window id (input reaching an Activity goes
 * to that window). */

/* pointer_lock modes (mirror of the Activity's CAPTURE_* in
 * WlWindowActivity.java — do not renumber, they cross the ctrl parcel) */
enum {
    AWL_CAPTURE_NONE    = 0,
    AWL_CAPTURE_CONFINE = 1,
    AWL_CAPTURE_LOCK    = 2,
};

enum {
    AWL_IN_PTR_ENTER = 1,    /* x,y view coordinates */
    AWL_IN_PTR_LEAVE = 2,
    AWL_IN_PTR_MOTION = 3,   /* x,y view coordinates */
    AWL_IN_PTR_BUTTON = 4,   /* code=BTN_*, v1=state */
    AWL_IN_PTR_AXIS = 5,     /* x=vertical, y=horizontal; code=0 wheel notches
                               (×10+discrete), code=1 touchpad finger pixel
                               distance (source=finger, raw value, no
                               discrete) */
    AWL_IN_PTR_REL = 6,      /* x,y relative motion (captured state: while a
                               lock/confine constraint is active the Activity
                               reports AXIS_RELATIVE_X/Y, otherwise the
                               absolute-position diff) */
    AWL_IN_KBD_ENTER = 7,    /* id gained keyboard focus */
    AWL_IN_KBD_LEAVE = 8,
    AWL_IN_KEY = 9,          /* code=evdev, v1=state(1/0) */
    AWL_IN_MODIFIERS = 10,   /* meta=Android meta bits */
    AWL_IN_TOUCH_DOWN = 11,  /* code=touch id, x,y */
    AWL_IN_TOUCH_MOTION = 12,
    AWL_IN_TOUCH_UP = 13,
    AWL_IN_TOUCH_CANCEL = 14,
    AWL_IN_TABLET_PROX_IN = 20,  /* zwp_tablet_v2 (stylus todo) */
    AWL_IN_TABLET_PROX_OUT = 21,
    AWL_IN_TABLET_MOTION = 22,   /* x,y v1=pressure tilt uses v2 */
    AWL_IN_TABLET_DOWN = 23,
    AWL_IN_TABLET_UP = 24,
    AWL_IN_TABLET_BUTTON = 25,
};

typedef struct awl_input_ev {
    uint64_t id;      /* target window (input reaching an Activity goes to that window) */
    uint32_t type;
    uint32_t code;    /* evdev code / touch id / tablet button */
    float x, y;       /* view coords | relative motion | wheel (v,h) */
    float v1, v2;     /* state / pressure / tilt… (per type) */
    uint32_t meta;    /* Android meta bits (for MODIFIERS) */
    uint32_t flags;   /* reserved (tool type etc.) */
} awl_input_ev_t;

void awl_input_dispatch(const awl_input_ev_t* ev);   /* any thread */

/* ---- IME text passthrough (Activity InputConnection → binder → client,
 *      any thread; same passthrough model as input: ops carry their window
 *      id, no routing state) ---- */

enum {
    AWL_IME_COMMIT = 1,     /* text: committed text (v3 commit_string; v1 same) */
    AWL_IME_PREEDIT = 2,    /* text: preedit; a/b = cursor_begin/end (byte offsets into text) */
    AWL_IME_DELETE = 3,     /* a/b = before/after (bytes; the client converts against its cache) */
    AWL_IME_CURSOR = 4,     /* a/b = index/anchor (v1 cursor_position; v3 ignores) */
};

/* ime_state flags */
#define AWL_IME_STATE_RESET 0x1u   /* v1 reset → Activity clears composing state + restartInput */

void awl_ime_text(uint64_t id, uint32_t op, const char* text, int32_t a, int32_t b);

/* ---- Android clipboard → wl selection (read by the focused Activity, then
 *      pushed over binder; any thread) ----
 * An internal source (text/plain;charset=utf-8) takes over the global
 * selection and notifies the focused client; an empty string clears the
 * selection. Echo suppression is the APK side's job (lastClipWritten). */
void awl_datadev_android_clip(const char* utf8);

/* ---- Adaptation layer → logic layer (render thread) ----
 * Frames reach the renderer through the surface's buffer queue
 * (awl_bufferqueue.h — dma-buf fd + acquire fence + geometry, nothing
 * else): every layer of a window has one. Per frame the renderer does, for
 * each layer, queue_ref → lock → drain → gethead → GL → set_release_fence →
 * unlock → unref. wl_shm buffers never enter it: the backend reads them in
 * place (awl_surface_shm_begin/end below). Buffer release / explicit-sync
 * release of dmabuf frames are the queue's business (they fire when a frame
 * leaves it), not the renderer's.
 *
 * awl_surface_queue_ref: +1 reference on the layer's queue (NULL = unknown
 * surface); the caller must awl_bufferqueue_unref it after the frame. The
 * reference keeps a locked queue alive across a concurrent surface death. */
struct awl_bufferqueue;
struct awl_bufferqueue* awl_surface_queue_ref(uint64_t id);

/* ---- wl_shm frame source (render thread) ----
 * A wl_shm commit is neither queued nor copied by the logic layer: it only
 * accumulates the surface's damage. The backend uploads the client's pixels
 * itself, straight from the pool into its own GPU texture, damage rect only
 * (one copy), at its frame cadence:
 *
 *   begin(id, have_serial, &f)
 *     0  the layer has no shm content (never committed, unmapped, or a
 *        dmabuf surface) — show nothing from shm
 *     2  shm content exists but nothing changed since `have_serial`, or the
 *        client already destroyed the wl_buffer (content persists: keep the
 *        last upload) — no lock held, do NOT call end
 *     1  *f describes the buffer to (re)upload; the surface is LOCKED until
 *        end (its client's commit waits, nothing else does): upload
 *        promptly, then end(consumed=1) — the damage accumulator resets and
 *        wl_buffer.release (+ the explicit-sync release object) goes to the
 *        client, i.e. "we are done reading". end(consumed=0) = the upload
 *        failed, everything stays pending.
 * f.damage: the rows/cols changed since the last consumed upload (dmg_full =
 * whole buffer; w/h 0 = nothing recorded — a consumer whose texture is new
 * uploads everything regardless). Any thread; `priv` is the lock handle. */
typedef struct awl_shm_frame {
    uint32_t width, height, stride;   /* stride in bytes */
    uint32_t format;                  /* DRM fourcc: 'AR24' / 'XR24' (memory order B,G,R,A|X) */
    const void* pixels;               /* valid until awl_surface_shm_end */
    int dmg_full;
    int32_t dmg_x, dmg_y, dmg_w, dmg_h;
    uint64_t serial;                  /* pass back as have_serial next time */
    void* priv;
} awl_shm_frame_t;
int  awl_surface_shm_begin(uint64_t id, uint64_t have_serial, awl_shm_frame_t* f);
void awl_surface_shm_end(awl_shm_frame_t* f, int consumed);

/* Adapter → logic layer: the Android window (render target) of root `id`
 * attached (1) / detached (0). While detached the commit path stops draining
 * the window's queues: the client runs out of buffers and parks — a
 * minimized window's client neither spins nor burns frames. Any thread. */
void awl_window_attached(uint64_t id, int attached);

/* ---- Sublayer composition snapshot (wl_subsurface, render thread) ----
 * #31 zoom: coordinates/sizes are always logical px (viewport dst | source |
 * buffer/scale); the render side scales dst by window-physical /
 * root-logical (=out[0].w). */

#define AWL_MAX_LAYERS 16

typedef struct awl_layer_info {
    uint64_t surface_id;   /* layer surface (bottom→top; the root is first only
                            * when no child is placed below it — kwin order) */
    float x, y;            /* root logical coordinates (Y down; root=(0,0)) */
    float w, h;            /* layer logical size (input hit-testing; 0 = no buffer on this layer) */
    float u0, v0, su, sv;  /* normalized uv transform of the sample region (viewport source; default = whole image) */
    int32_t transform;     /* wl_surface.set_buffer_transform (wl_output.transform 0..7,
                            * applied on commit; 90/270 swap the logical size) */
} awl_layer_info_t;

/* Returns the layer count in render order bottom→top. The order is the
 * wl_subsurface stacking model as kwin traverses it (SurfaceInterface::
 * traverseTree / SurfaceItemWayland z): for every surface its below-children
 * (each with its own subtree), then the surface itself, then its
 * above-children. A child placed below its parent (place_below parent) is
 * therefore emitted BEFORE the parent. No root / over the limit → truncated
 * (>0 is enough to render). */
int  awl_surface_get_layers(uint64_t root_id, awl_layer_info_t* out, int max);

/* Client cursor layer of this window (wl_pointer.set_cursor surface; render
 * thread). Returns 1 and fills *out when the pointer-focused client set a
 * cursor surface for this window: the renderer composites it ABOVE every
 * layer returned by awl_surface_get_layers (it is not part of that stack and
 * never hit-tests). x,y = pointer position − hotspot (root logical
 * coordinates, same basis as the layer stack), w,h = cursor surface logical
 * size. 0 = nothing to draw (no cursor set, set_cursor(NULL) = invisible
 * pointer, or the pointer is in another window). */
int  awl_pointer_cursor_layer(uint64_t root_id, awl_layer_info_t* out);

/* Root view transform snapshot for the render thread: xdg geometry origin
 * (logical px; never set = 0,0) + the logical→view mapping
 * view = (logical − origin) × s + o, decided per root (1:1 at the zoom for
 * content following the configure, scale_mode placement otherwise) — the
 * same numbers the input inverse uses, so render and hit-test cannot drift.
 * Consumers snap the resulting rects to the pixel grid (integer origin, size
 * = round(logical × s)) so a buffer of round(logical × Z) px covers exactly
 * its own pixel count. double like kwin's qreal: at an exact half-pixel tie
 * (logical × preferred_scale ≡ 60 mod 120, e.g. 2265 × 124/120 = 2340.5) a
 * float32 product lands just below .5 and rounds the other way than the
 * client's arithmetic — a 1 px stretch on that axis. Unknown root →
 * identity. Any thread. */
typedef struct awl_view_xform {
    int32_t gox, goy;      /* geometry origin (logical px) */
    double sx, sy;         /* logical → view scale */
    double ox, oy;         /* view offset (letterbox centering; 0 when 1:1) */
} awl_view_xform_t;
void awl_surface_get_view_xform(uint64_t root_id, awl_view_xform_t* out);

/* This frame has been presented (rendering done; the render thread sends
 * the frame callbacks directly) */
void awl_surface_presented(uint64_t id);

#ifdef __cplusplus
}
#endif
#endif /* AWL_H */
