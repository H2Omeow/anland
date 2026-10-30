/*
 * vk_layer_anland_vr.c - Vulkan implicit layer that keeps SteamVR windowed.
 *
 * anland composites the XR window on the host: vrcompositor renders into a
 * regular Wayland toplevel and the daemon applies the lens distortion while it
 * blits that window to the phone/glasses. That only works while vrcompositor
 * presents through a real surface; if it decides to take a DRM display over
 * VK_KHR_display the host never sees the frames.
 *
 * Turnip/KGSL make this worse: they advertise VK_KHR_display but fail to
 * enumerate a physical device once a display extension is used, which looks
 * like "no GPU" to vrcompositor.
 *
 * So: while an XR session is configured (see anland-xr-display), take the
 * display extensions off the instance and device create calls. Everything else
 * is passed straight through - this layer adds no dispatch overhead beyond
 * vkCreateInstance/vkCreateDevice and the two proc-addr entry points.
 *
 * The layer is enabled by VK_LAYER_anland_vr.json's enable_environment
 * (ANLAND_VR_LAYER_ENABLE=1), which xr/anland-steamvr sets, so ordinary desktop
 * Vulkan work in the container is untouched.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LAYER_NAME "VK_LAYER_anland_vr"
#define EXPORT __attribute__((visibility("default")))

/* display-round extensions: instance level */
static const char *const kDisplayInstanceExts[] = {
    "VK_KHR_display",
    "VK_KHR_display_2",
    "VK_EXT_direct_mode_display",
    "VK_EXT_display_surface_counter",
    "VK_EXT_display_control",
    "VK_KHR_get_display_properties2",
    "VK_EXT_acquire_xlib_display",
    "VK_EXT_acquire_drm_display",
};

/* ... and the one device-level extension that goes with them */
static const char *const kDisplayDeviceExts[] = {
    "VK_KHR_display_swapchain",
};

static int is_display_ext(const char *name, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(name, list[i])) return 1;
    return 0;
}

static void say(const char *fmt, const char *arg) {
    if (getenv("ANLAND_VR_LAYER_QUIET")) return;
    fprintf(stderr, "[anland-vr] ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
}

/*
 * Strip only while the daemon has an XR session configured: read mode= out of
 * anland-xr-display. The daemon rewrites that file atomically, so a short read
 * just means "not yet".
 */
static int xr_wanted(void) {
    if (getenv("ANLAND_VR_LAYER_DISABLE")) return 0;
    const char *rt = getenv("ANLAND_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/run/anland";

    char path[512];
    snprintf(path, sizeof(path), "%s/anland-xr-display", rt);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    const char *m = strstr(buf, "mode=");
    if (!m) return 0;
    return atoi(m + 5) != 0;
}

struct inst {
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    int stripped;
};
struct dev {
    VkDevice handle;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
};

#define MAX_OBJS 64
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct inst g_inst[MAX_OBJS];
static struct dev g_dev[MAX_OBJS];

/* the loader hands out the same pointer value for a handle; compare that */
static void *key(const void *obj) { return *(void **)obj; }

static struct inst *inst_for(const void *obj) {
    struct inst *r = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_OBJS; i++)
        if (g_inst[i].handle && key(g_inst[i].handle) == key(obj)) { r = &g_inst[i]; break; }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static struct dev *dev_for(const void *obj) {
    struct dev *r = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_OBJS; i++)
        if (g_dev[i].handle && key(g_dev[i].handle) == key(obj)) { r = &g_dev[i]; break; }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static VkLayerInstanceCreateInfo *instance_chain(const VkInstanceCreateInfo *ci) {
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    return chain;
}

static VkLayerDeviceCreateInfo *device_chain(const VkDeviceCreateInfo *ci) {
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO))
        chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    return chain;
}

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc,
                                          VkInstance *out) {
    VkLayerInstanceCreateInfo *chain = instance_chain(ci);
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateInstance next_create =
        (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    VkInstanceCreateInfo mine = *ci;
    const char **names = NULL;
    uint32_t n = 0;
    int stripped = 0;

    if (xr_wanted() && ci->enabledExtensionCount) {
        names = (const char **)calloc(ci->enabledExtensionCount + 1, sizeof(*names));
        if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
            const char *e = ci->ppEnabledExtensionNames[i];
            if (is_display_ext(e, kDisplayInstanceExts,
                               sizeof(kDisplayInstanceExts) / sizeof(*kDisplayInstanceExts))) {
                say("hiding %s: XR session keeps SteamVR windowed", e);
                stripped = 1;
                continue;
            }
            names[n++] = e;
        }
        mine.enabledExtensionCount = n;
        mine.ppEnabledExtensionNames = names;
    }

    VkResult r = next_create(&mine, alloc, out);
    free(names);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_OBJS; i++) {
        if (!g_inst[i].handle) {
            g_inst[i].handle = *out;
            g_inst[i].gipa = next_gipa;
            g_inst[i].destroy =
                (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
            g_inst[i].stripped = stripped;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyInstance(VkInstance instance,
                                       const VkAllocationCallbacks *alloc) {
    struct inst *in = inst_for(instance);
    if (!in) return;
    PFN_vkDestroyInstance d = in->destroy;
    pthread_mutex_lock(&g_lock);
    memset(in, 0, sizeof(*in));
    pthread_mutex_unlock(&g_lock);
    if (d) d(instance, alloc);
}

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd,
                                        const VkDeviceCreateInfo *ci,
                                        const VkAllocationCallbacks *alloc,
                                        VkDevice *out) {
    VkLayerDeviceCreateInfo *chain = device_chain(ci);
    if (!chain) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetDeviceProcAddr next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateDevice next_create =
        (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo mine = *ci;
    const char **names = NULL;
    uint32_t n = 0;

    if (xr_wanted() && ci->enabledExtensionCount) {
        names = (const char **)calloc(ci->enabledExtensionCount + 1, sizeof(*names));
        if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
            const char *e = ci->ppEnabledExtensionNames[i];
            if (is_display_ext(e, kDisplayDeviceExts,
                               sizeof(kDisplayDeviceExts) / sizeof(*kDisplayDeviceExts))) {
                say("hiding %s on the device", e);
                continue;
            }
            names[n++] = e;
        }
        mine.enabledExtensionCount = n;
        mine.ppEnabledExtensionNames = names;
    }

    VkResult r = next_create(pd, &mine, alloc, out);
    free(names);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_OBJS; i++) {
        if (!g_dev[i].handle) {
            g_dev[i].handle = *out;
            g_dev[i].gdpa = next_gdpa;
            g_dev[i].destroy = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return VK_SUCCESS;
}

static void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    struct dev *d = dev_for(device);
    if (!d) return;
    PFN_vkDestroyDevice fn = d->destroy;
    pthread_mutex_lock(&g_lock);
    memset(d, 0, sizeof(*d));
    pthread_mutex_unlock(&g_lock);
    if (fn) fn(device, alloc);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AnlandVr_GetDeviceProcAddr(VkDevice device, const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AnlandVr_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)CreateDevice;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)AnlandVr_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)AnlandVr_GetDeviceProcAddr;
    if (!instance) return NULL;

    struct inst *in = inst_for(instance);
    return in ? in->gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
AnlandVr_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)DestroyDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)AnlandVr_GetDeviceProcAddr;

    struct dev *d = dev_for(device);
    return d ? d->gdpa(device, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
AnlandVr_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = AnlandVr_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = AnlandVr_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
