/* awl_log.h — logging macros shared by every native layer
 *
 * LOGI  lifecycle milestones (client/window creation and teardown, state
 *       transitions, one-shot init diagnostics) — kept in release builds
 * LOGW  a configuration or peer behaviour we can live with but the user
 *       should know about (one-shot; always kept) — e.g. an eye target whose
 *       aspect no longer matches the window it is mapped onto
 * LOGE  errors and abnormal conditions — always kept
 * LOGD  hot-path tracing (event-loop dispatch: per-attach / per-commit /
 *       per-frame / per-buffer requests, render-thread buffer imports) —
 *       compiled out unless AWL_LOG_DEBUG is defined. Release builds (the
 *       Makefile default) do not define it; `make native-debug` (or cmake
 *       -DAWL_LOG_DEBUG=ON) builds it back in. When compiled out, the
 *       arguments are not evaluated.
 *
 * Per-file tag: #define AWL_TAG "..." before including this header
 * (default "anland-wl").
 */
#ifndef AWL_LOG_H
#define AWL_LOG_H

#include <android/log.h>

#ifndef AWL_TAG
#define AWL_TAG "anland-wl"
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  AWL_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  AWL_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, AWL_TAG, __VA_ARGS__)

#ifdef AWL_LOG_DEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, AWL_TAG, __VA_ARGS__)
#else
#define LOGD(...) do {} while (0)
#endif

/* AWL_ASSERT(cond) — invariant tripwire, not error handling: LOGE a warning
 * in debug builds (CMAKE_BUILD_TYPE=Debug, or any build with AWL_LOG_DEBUG
 * tracing), compile to nothing in release. Never aborts — execution
 * continues as if the guard were still there, so converting a defensive
 * check to AWL_ASSERT is only sound when the condition is provably
 * unreachable (documented invariant); conditions a peer thread or client
 * input can legitimately make false must stay real guards. Like LOGD, the
 * argument is not evaluated when compiled out — keep it side-effect free. */
#if !defined(NDEBUG) || defined(AWL_LOG_DEBUG)
#define AWL_ASSERT(cond) do { \
    if (!(cond)) \
        LOGE("ASSERT %s:%d: invariant broken: %s", __FILE__, __LINE__, #cond); \
} while (0)
#else
#define AWL_ASSERT(cond) do {} while (0)
#endif

#endif /* AWL_LOG_H */
