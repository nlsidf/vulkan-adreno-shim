/*
 * vulkan_gpu.so — Vulkan 版 namespace 绕过库 (Adreno GPU)
 *
 * 复用 egl_gpu.so 的链接逻辑: 在构造函数中把 default namespace 与
 * sphal namespace 链接, 放行 Adreno/Vulkan 相关的 /vendor 库, 使
 * Mesa 的 Vulkan loader 能 dlopen Adreno Vulkan 驱动及其依赖。
 *
 * 编译:
 *   gcc -shared -fPIC -o vulkan_gpu.so vulkan_gpu.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

struct android_namespace_t;
typedef struct android_namespace_t android_namespace_t;
typedef bool (*link_ns_func_t)(android_namespace_t*, android_namespace_t*, const char*);
typedef android_namespace_t* (*get_ns_func_t)(const char*);

static int g_debug = -1;
static int is_debug(void) {
    if (g_debug < 0) {
        const char* e = getenv("VK_GPU_LOG");
        g_debug = (e && e[0]) ? atoi(e) : 0;
    }
    return g_debug;
}
#define LOG(...) do { if (is_debug()) fprintf(stderr, "[VK_GPU] " __VA_ARGS__); } while(0)
#define WARN(...) do { fprintf(stderr, "[VK_GPU] 警告: " __VA_ARGS__); } while(0)

static void link_namespaces(void) {
    get_ns_func_t get_ns = (get_ns_func_t)dlsym(RTLD_DEFAULT,
        "__loader_android_get_exported_namespace");
    link_ns_func_t link_ns = (link_ns_func_t)dlsym(RTLD_DEFAULT,
        "__loader_android_link_namespaces");

    if (get_ns && link_ns) {
        android_namespace_t* def_ns = get_ns("default");
        android_namespace_t* sphal_ns = get_ns("sphal");
        if (def_ns && sphal_ns) {
            /* 放行 Adreno/Vulkan 驱动及其全部 /vendor 依赖 */
            const char* libs =
                "vulkan.msm8998.so:libEGL_adreno.so:libGLESv2_adreno.so:libGLESv1_CM_adreno.so:"
                "libq3dtools_adreno.so:libadreno_utils.so:libgsl.so:libllvm-glnext.so:"
                "libcutils.so:libutils.so:libhardware.so:libnativewindow.so:"
                "libvulkan.so:libvkjson.so:libsync.so";
            link_ns(def_ns, sphal_ns, libs);
            LOG("namespace 链接: default <-> sphal (Adreno/Vulkan)");
        } else {
            WARN("namespace 获取失败 (def=%p, sphal=%p)", (void*)def_ns, (void*)sphal_ns);
        }
    } else {
        WARN("namespace 链接不可用 (非 Android 环境)");
    }
}

__attribute__((constructor))
static void vulkan_gpu_constructor(void) {
    link_namespaces();
}

/* 保留符号, 避免被裁剪 */
void vulkan_gpu_noop(void) { link_namespaces(); }
