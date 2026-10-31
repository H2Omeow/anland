/* awl_present.c - wp_presentation (presentation-time) server side
 *
 * Why this exists: a nested compositor (KWin, weston, sway, mutter...) owns
 * its own render loop, and that loop is driven by two pieces of information
 * the host has to supply - the frame callback (when to start the next frame)
 * and, if advertised, the presentation feedback (when the frame it just
 * submitted actually reached the screen, at which refresh, with which
 * sequence number). Without wp_presentation a compositor can only assume
 * "frame callback fired == previous frame is on screen, at some unknown
 * refresh rate", so it over-submits buffers and its paced/animated output
 * drifts. That is the KDE mailbox problem: the frame it waits on is
 * unrelated to the frame it submitted.
 *
 * Where the timestamps come from: the renderer presents on its own thread
 * and then calls awl_surface_presented(); that is the single point where a
 * frame becomes the current one, so it is also where the feedback objects
 * attached to that surface are answered. Timestamps are CLOCK_MONOTONIC and
 * describe the moment eglSwapBuffers() returned - the best estimate the host
 * has without EGL_ANDROID_get_frame_timestamps (using that would need the
 * surface frame id, which the SC backend never produces).
 *
 * Lock order: this module's g_lock is a leaf. awl_present_frame() and
 * awl_present_surface_gone() may run under g_srv.rwl(rd) + s->ev_lock (the
 * render thread's present path) or with no lock at all (teardown); nothing
 * here ever takes rwl, so the order rwl -> ev_lock -> g_lock is preserved.
 *
 * Protocol objects are keyed by window id, not by awl_surface*, because the
 * id is stable across the lifetime of the feedback object while the surface
 * pointer is not (ids are never reused: g_srv.next_surface_id only grows). */

#define AWL_TAG "anland-pres"
#include "awl_internal.h"

#include <string.h>

#include "presentation-time-server-protocol.h"

/* A client can queue feedback objects faster than frames arrive (first frame
 * after a cold start, or a paused surface). Cap them: past the cap a request
 * is answered with 'discarded' immediately, which is always legal and keeps
 * the list bounded even for a hostile client. */
#define AWL_PRES_MAX_PENDING 64

/* wp_presentation.clock_id is an enum in the protocol; CLOCK_MONOTONIC is 1
 * on every Linux ABI we build for, but spell it out instead of assuming. */
#define AWL_PRES_CLOCK_ID CLOCK_MONOTONIC

struct pres_fb {
    struct wl_list link;            /* g_pending (g_lock) */
    struct wl_resource* res;        /* wp_presentation_feedback (the request's new_id) */
    uint64_t surface_id;            /* window the feedback was requested for */
};

static struct wl_list g_pending;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;
static uint32_t g_refresh_ns;       /* frame period of the reported output, ns */
static uint32_t g_seq;              /* presented counter; protocol says u32 per output */

static uint64_t pres_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Answer one feedback object. Called without g_lock (the object has already
 * been unlinked, so the answer is the only remaining reference). Both
 * 'presented' and 'discarded' are destructor events: the resource is gone
 * afterwards on both sides. */
static void pres_answer(struct pres_fb* f, uint64_t when_ns, int discarded) {
    uint32_t sec = (uint32_t)(when_ns / 1000000000ull);
    uint32_t nsec = (uint32_t)(when_ns % 1000000000ull);
    uint32_t seq = __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELAXED);

    if (discarded) {
        wp_presentation_feedback_send_discarded(f->res);
    } else {
        wp_presentation_feedback_send_presented(
                f->res, 0u, sec, nsec, g_refresh_ns, 0u, seq, 0u);
    }
    wl_resource_destroy(f->res);
}

/* Take every pending feedback object of one window off the list. Returns how
 * many were stored in 'out' (never more than AWL_PRES_MAX_PENDING). */
static int pres_take(uint64_t surface_id, struct pres_fb** out) {
    int n = 0;
    struct pres_fb* f;
    struct pres_fb* tmp;
    pthread_mutex_lock(&g_lock);
    wl_list_for_each_safe(f, tmp, &g_pending, link) {
        if (f->surface_id != surface_id) continue;
        wl_list_remove(&f->link);
        out[n++] = f;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ---------------- resource lifecycle ---------------- */

static void pres_fb_res_destroy(struct wl_resource* res) {
    struct pres_fb* f = wl_resource_get_user_data(res);
    if (!f) return;
    pthread_mutex_lock(&g_lock);
    wl_list_remove(&f->link);
    pthread_mutex_unlock(&g_lock);
    f->res = NULL;
    free(f);
}

static void pres_feedback(struct wl_client* client, struct wl_resource* res,
                          struct wl_resource* surface_res, uint32_t id) {
    struct awl_surface* s = awl_surface_from_res(surface_res);
    /* wp_presentation_feedback has no requests in v1 - it only carries
     * events (sync_output, presented, discarded) and self-destructs when one
     * of the terminal two is sent, so there is no implementation table. */
    struct wl_resource* fb_res = wl_resource_create(
            client, &wp_presentation_feedback_interface, 1, id);
    if (!fb_res) { wl_client_post_no_memory(client); return; }

    struct pres_fb* f = calloc(1, sizeof(*f));
    if (!f) {
        wl_resource_destroy(fb_res);
        wl_client_post_no_memory(client);
        return;
    }
    f->res = fb_res;
    f->surface_id = s ? s->id : 0;
    wl_resource_set_implementation(fb_res, NULL, f, pres_fb_res_destroy);

    if (!s) {
        /* unknown surface: no frame will ever be presented for it */
        pres_answer(f, pres_mono_ns(), 1);
        return;
    }

    int over = 0;
    pthread_mutex_lock(&g_lock);
    int pending = 0;
    struct pres_fb* it;
    wl_list_for_each(it, &g_pending, link) pending++;
    if (pending >= AWL_PRES_MAX_PENDING) over = 1;
    else wl_list_insert(g_pending.prev, &f->link);
    pthread_mutex_unlock(&g_lock);
    if (over) pres_answer(f, pres_mono_ns(), 1);
}

static void pres_destroy(struct wl_client* client, struct wl_resource* res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct wp_presentation_interface pres_iface = {
    .destroy = pres_destroy,
    .feedback = pres_feedback,
};

static void pres_bind(struct wl_client* client, void* data,
                      uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource* res = wl_resource_create(
            client, &wp_presentation_interface, version < 1 ? version : 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &pres_iface, NULL, NULL);
    /* Say which clock the timestamps use before any feedback is answered. */
    wp_presentation_send_clock_id(res, (uint32_t)AWL_PRES_CLOCK_ID);
}

/* ---------------- daemon-facing API ---------------- */

void awl_present_set_refresh(double hz) {
    uint32_t ns = 0;
    if (hz > 0.0) ns = (uint32_t)(1000000000.0 / hz + 0.5);
    if (ns == 0) ns = 16666667u;      /* 60 Hz fallback */
    pthread_mutex_lock(&g_lock);
    g_refresh_ns = ns;
    pthread_mutex_unlock(&g_lock);
}

void awl_present_setup(void) {
    wl_list_init(&g_pending);
    double hz = g_srv.info.refresh_hz > 0 ? g_srv.info.refresh_hz : 60.0;
    uint32_t ns = (uint32_t)(1000000000.0 / hz + 0.5);
    pthread_mutex_lock(&g_lock);
    g_refresh_ns = ns ? ns : 16666667u;
    pthread_mutex_unlock(&g_lock);

    if (!wl_global_create(g_srv.display, &wp_presentation_interface, 1,
                          NULL, pres_bind)) {
        LOGE("wp_presentation: wl_global_create failed");
        return;
    }
    g_ready = 1;
    LOGI("presentation-time: wp_presentation v1 advertised, refresh %.3f Hz",
         hz);
}

/* Render thread, after eglSwapBuffers: the frame for this window is now the
 * one on screen, so every feedback queued for it can be answered. */
void awl_present_frame(uint64_t surface_id) {
    if (!g_ready) return;
    struct pres_fb* batch[AWL_PRES_MAX_PENDING];
    uint64_t now = pres_mono_ns();
    int n = pres_take(surface_id, batch);
    for (int i = 0; i < n; i++) {
        pres_answer(batch[i], now, 0);
        free(batch[i]);
    }
}

/* Window gone (or never mapped): answer outstanding feedback with
 * 'discarded' - the protocol's way of saying "this frame will not appear". */
void awl_present_surface_gone(uint64_t surface_id) {
    if (!g_ready) return;
    struct pres_fb* batch[AWL_PRES_MAX_PENDING];
    uint64_t now = pres_mono_ns();
    int n = pres_take(surface_id, batch);
    for (int i = 0; i < n; i++) {
        pres_answer(batch[i], now, 1);
        free(batch[i]);
    }
}
