/* awl_xdgout.c - zxdg_output_manager_v1 (xdg-output-unstable-v1) server side
 *
 * Why this exists: wl_output alone reports a mode in *physical* pixels and a
 * physical size in millimetres, which is enough for a plain app but not for a
 * compositor host. KWin, weston and sway all publish their own outputs (and
 * their Qt/X11 counterparts' screen geometry) from the xdg-output logical
 * geometry, and Qt (KWin is Qt) gates several code paths - screen name for
 * config matching, per-output logical size for the virtual screen - on
 * zxdg_output_v1 being present at all. Without it a nested KWin falls back to
 * "one anonymous output of the wl_output mode", and Plasma's output
 * configuration can mis-scale or pick the wrong default.
 *
 * The host's logical space is identical to its physical one (anland does not
 * scale the desktop: zoom is per-window in the renderer), so logical_size ==
 * wl_output mode, logical_position == 0,0 and the name is a constant. That is
 * deliberately boring: what matters is that the objects and their 'done'
 * exist and are re-sent whenever the output changes.
 *
 * Lock order: nothing here takes g_srv.rwl. awl_xdgout_output_changed() is
 * called by awl_server.c with rwl already held (it re-uses the same
 * re-announce path as wl_output), so this module's own list lock is a leaf
 * like awl_present.c's. */

#define AWL_TAG "anland-out"
#include "awl_internal.h"

#include <string.h>

#include "xdg-output-unstable-v1-server-protocol.h"

#define AWL_XDGOUT_NAME        "anland-0"
#define AWL_XDGOUT_DESCRIPTION "anland virtual output"

struct xdgout {
    struct wl_resource* res;        /* zxdg_output_v1 */
    struct wl_resource* output_res; /* the wl_output it was created from */
    struct wl_list link;            /* g_xdgouts (g_lock) */
};

static struct wl_list g_xdgouts;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;

/* Send the full state. Caller holds g_lock (and, through the daemon, rwl:
 * g_srv.info is read here exactly like wl_output's own re-announce does). */
static void xdgout_send_state_locked(struct xdgout* x) {
    uint32_t v = wl_resource_get_version(x->res);

    zxdg_output_v1_send_logical_position(x->res, 0, 0);
    zxdg_output_v1_send_logical_size(x->res,
                                     (int32_t)g_srv.info.width,
                                     (int32_t)g_srv.info.height);
    if (v >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION)
        zxdg_output_v1_send_name(x->res, AWL_XDGOUT_NAME);
    if (v >= ZXDG_OUTPUT_V1_DESCRIPTION_SINCE_VERSION)
        zxdg_output_v1_send_description(x->res, AWL_XDGOUT_DESCRIPTION);
    /* v3 deprecated 'done' (wl_output.done covers it) but v1/v2 clients,
     * including Qt's, wait for it. Send it where the protocol allows. */
    if (v < ZXDG_OUTPUT_V1_DONE_SINCE_VERSION)
        zxdg_output_v1_send_done(x->res);
}

static void xdgout_send_state(struct xdgout* x) {
    pthread_mutex_lock(&g_lock);
    xdgout_send_state_locked(x);
    pthread_mutex_unlock(&g_lock);
}

static void xdgout_res_destroy(struct wl_resource* res) {
    struct xdgout* x = wl_resource_get_user_data(res);
    if (!x) return;
    pthread_mutex_lock(&g_lock);
    wl_list_remove(&x->link);
    pthread_mutex_unlock(&g_lock);
    free(x);
}

static void xdgout_destroy(struct wl_client* client, struct wl_resource* res) {
    (void)client;
    wl_resource_destroy(res);
}

static const struct zxdg_output_v1_interface xdgout_iface = {
    .destroy = xdgout_destroy,
};

static void xdgoutmgr_destroy(struct wl_client* client, struct wl_resource* res) {
    (void)client;
    wl_resource_destroy(res);
}

static void xdgoutmgr_get_xdg_output(struct wl_client* client,
                                     struct wl_resource* res,
                                     uint32_t id,
                                     struct wl_resource* output_res) {
    if (!awl_output_res_owns(output_res)) {
        /* this protocol revision has no error enum: dropping the request
         * leaves the client without an object, which is the right outcome for
         * a foreign wl_output (only this daemon's single output is real). */
        LOGD("get_xdg_output: ignoring foreign wl_output");
        return;
    }
    struct wl_resource* xres = wl_resource_create(
            client, &zxdg_output_v1_interface, wl_resource_get_version(res), id);
    if (!xres) { wl_client_post_no_memory(client); return; }

    struct xdgout* x = calloc(1, sizeof(*x));
    if (!x) {
        wl_resource_destroy(xres);
        wl_client_post_no_memory(client);
        return;
    }
    x->res = xres;
    x->output_res = output_res;
    wl_resource_set_implementation(xres, &xdgout_iface, x, xdgout_res_destroy);

    pthread_mutex_lock(&g_lock);
    wl_list_insert(g_xdgouts.prev, &x->link);
    xdgout_send_state_locked(x);
    pthread_mutex_unlock(&g_lock);
    wl_client_flush(client);
}

static const struct zxdg_output_manager_v1_interface xdgoutmgr_iface = {
    .destroy = xdgoutmgr_destroy,
    .get_xdg_output = xdgoutmgr_get_xdg_output,
};

static void xdgoutmgr_bind(struct wl_client* client, void* data,
                           uint32_t version, uint32_t id) {
    (void)data;
    /* xdg-output-unstable-v1 as shipped in protocols/ is at spec version 3;
     * wayland-scanner does not emit an interface-version macro for the
     * manager, so the number is written out here. */
    uint32_t v = version < 3 ? version : 3;
    struct wl_resource* res = wl_resource_create(
            client, &zxdg_output_manager_v1_interface, v, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &xdgoutmgr_iface, NULL, NULL);
}

/* ---------------- daemon-facing API ---------------- */

void awl_xdgout_setup(void) {
    wl_list_init(&g_xdgouts);
    if (!wl_global_create(g_srv.display, &zxdg_output_manager_v1_interface,
                          3, NULL, xdgoutmgr_bind)) {
        LOGE("zxdg_output_manager_v1: wl_global_create failed");
        return;
    }
    g_ready = 1;
    LOGI("xdg-output: zxdg_output_manager_v1 v3 advertised");
}

/* The output's state changed (window grew, mode re-announced). awl_server.c
 * calls this with rwl held, right after it re-sent wl_output state; the
 * clients that create xdg-output objects after this point get the new size
 * from their own get_xdg_output call. */
void awl_xdgout_output_changed(void) {
    if (!g_ready) return;
    pthread_mutex_lock(&g_lock);
    struct xdgout* x;
    wl_list_for_each(x, &g_xdgouts, link) {
        xdgout_send_state_locked(x);
        wl_client_flush(wl_resource_get_client(x->res));
    }
    pthread_mutex_unlock(&g_lock);
}
