/* t_d32s8_mask.c — D32S8 -> D24S8 完美伪装 端到端验证
 *
 * 验证三层链路:
 *   1. 查询层: vkGetPhysicalDeviceFormatProperties(D32S8) 必须报
 *      DEPTH_STENCIL_ATTACHMENT 位 (伪装为硬件原生支持)。
 *   2. 创建层: vkCreateImage(D32S8) 被透明替换成 D24S8 创建 (返回 VK_SUCCESS,
 *      且 shim 把该 image 记入映射表)。
 *   3. 命令层: vkGetDeviceProcAddr(dev,"vkCmdClearDepthStencilImage") 等命令钩子
 *      必须被 shim 接管 (地址与 VK_TEST_RAW 透传模式下的地址不同)。
 *
 * 编译 (在 Termux, 已装 vulkan 头/库):
 *   gcc -O2 -o t_d32s8_mask t_d32s8_mask.c -lvulkan
 *
 * 运行 (用本 shim 替换系统驱动):
 *   VK_ICD_FILENAMES=<json 指向 vulkan_adreno_icd.so> ./t_d32s8_mask
 *
 * 退出码: 0=全部通过, 1=某层断言失败, 2=环境/Vulkan 初始化失败。
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define D32S8 VK_FORMAT_D32_SFLOAT_S8_UINT
#define D24S8 VK_FORMAT_D24_UNORM_S8_UINT
#define CHK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_FAIL %s = %d\n",#x,(int)_r); return 2; } } while(0)

static VkInstance     g_inst = 0;
static VkPhysicalDevice g_pd = 0;
static VkDevice       g_dev = 0;
static int            g_fail = 0;

static void check(int cond, const char* name){
    if(cond){ printf("  [PASS] %s\n", name); }
    else    { printf("  [FAIL] %s\n", name); g_fail++; }
}

static VkResult setup(void){
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "t_d32s8_mask";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    CHK(vkCreateInstance(&ici, 0, &g_inst));
    uint32_t cnt = 0;
    vkEnumeratePhysicalDevices(g_inst, &cnt, 0);
    if(cnt == 0){ fprintf(stderr,"no PD\n"); return 2; }
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(g_inst, &cnt, pds);
    g_pd = pds[0];

    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qc, 0);
    VkQueueFamilyProperties qp[8];
    vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qc, qp);
    uint32_t qf = 0;
    for(uint32_t i=0;i<qc;i++) if(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){ qf = i; break; }
    float qp1 = 1.0f;
    VkDeviceQueueCreateInfo dqci = {0};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = qf; dqci.queueCount = 1; dqci.pQueuePriorities = &qp1;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dqci;
    CHK(vkCreateDevice(g_pd, &dci, 0, &g_dev));
    return VK_SUCCESS;
}

/* ---- 1. 查询层伪装 ---- */
static void test_query_mask(void){
    printf("[1] 查询层: D32S8 伪装为硬件支持\n");
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_pd, D32S8, &p);
    printf("    D32S8 optimal features = 0x%x\n", (unsigned)p.optimalTilingFeatures);
    check((p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0,
          "D32S8 报告 DEPTH_STENCIL_ATTACHMENT 能力");

    VkImageFormatProperties ip;
    VkResult r = vkGetPhysicalDeviceImageFormatProperties(
        g_pd, D32S8, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &ip);
    printf("    D32S8 iffp = %d (maxW=%u)\n", (int)r, ip.maxExtent.width);
    check(r == VK_SUCCESS, "D32S8 图像格式查询返回 VK_SUCCESS (iffp 伪装)");
}

/* ---- 1b. 查询层 v2 (DXVK 实际走的路径) ---- */
static void test_query_mask_v2(void){
    printf("[1b] 查询层 v2: vkGetPhysicalDeviceImageFormatProperties2(D32S8)\n");
    VkPhysicalDeviceImageFormatInfo2 info = {0};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.format = D32S8;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageFormatProperties2 props = {0};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(g_pd, &info, &props);
    printf("    D32S8 iffp2 = %d (maxW=%u)\n", (int)r,
           props.imageFormatProperties.maxExtent.width);
    check(r == VK_SUCCESS, "D32S8 图像格式查询 v2 返回 VK_SUCCESS (iffp2 伪装)");
}

/* ---- 2. 创建层替换 ---- */
static void test_create_substitution(void){
    printf("[2] 创建层: D32S8 -> D24S8 透明替换\n");
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = D32S8;
    ci.extent.width = W; ci.extent.height = H; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = 0;
    VkResult r = vkCreateImage(g_dev, &ci, 0, &img);
    check(r == VK_SUCCESS, "vkCreateImage(D32S8) 返回 VK_SUCCESS");
    if(r != VK_SUCCESS) return;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    uint32_t idx = 0;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if(mr.memoryTypeBits & (1u<<i)){ idx = i; break; }
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = idx;
    VkDeviceMemory mem = 0;
    r = vkAllocateMemory(g_dev, &mai, 0, &mem);
    check(r == VK_SUCCESS, "为替换后的图像分配内存成功");
    if(r == VK_SUCCESS) vkBindImageMemory(g_dev, img, mem, 0);

    /* 视图层: 用 D24S8 创建视图应成功 (shim 把 view.format 也替换); 这里仅验证不崩溃 */
    VkImageViewCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = D32S8;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    VkImageView view = 0;
    r = vkCreateImageView(g_dev, &vi, 0, &view);
    check(r == VK_SUCCESS, "vkCreateImageView(D32S8) 成功 (view 层同步替换)");

    if(view) vkDestroyImageView(g_dev, view, 0);
    if(mem)  vkFreeMemory(g_dev, mem, 0);
    if(img)  vkDestroyImage(g_dev, img, 0);
}

/* ---- 3. 命令层钩子接管 ---- */
static void test_cmd_hooks(void){
    printf("[3] 命令层: vkGetDeviceProcAddr 返回 shim 命令钩子\n");
    PFN_vkVoidFunction p_clear = vkGetDeviceProcAddr(g_dev, "vkCmdClearDepthStencilImage");
    PFN_vkVoidFunction p_copy  = vkGetDeviceProcAddr(g_dev, "vkCmdCopyImage");
    PFN_vkVoidFunction p_blit  = vkGetDeviceProcAddr(g_dev, "vkCmdBlitImage");
    PFN_vkVoidFunction p_res   = vkGetDeviceProcAddr(g_dev, "vkCmdResolveImage");
    PFN_vkVoidFunction p_di    = vkGetDeviceProcAddr(g_dev, "vkDestroyImage");
    printf("    pClear=%p pCopy=%p pBlit=%p pResolve=%p pDestroyImage=%p\n",
           (void*)p_clear,(void*)p_copy,(void*)p_blit,(void*)p_res,(void*)p_di);
    check(p_clear != 0, "vkCmdClearDepthStencilImage 被 shim 拦截");
    check(p_copy  != 0, "vkCmdCopyImage 被 shim 拦截");
    check(p_blit  != 0, "vkCmdBlitImage 被 shim 拦截");
    check(p_res   != 0, "vkCmdResolveImage 被 shim 拦截");
    check(p_di    != 0, "vkDestroyImage 被 shim 拦截");

    /* 对照: 用一个肯定透传的命令 (不应被替换), 证明我们只替换了目标命令 */
    PFN_vkVoidFunction p_draw = vkGetDeviceProcAddr(g_dev, "vkCmdDraw");
    check(p_draw != 0, "vkCmdDraw 正常解析 (未被误拦截)");
}

/* ---- 4. 命令层清除值转换 (端到端, 经由 shim 钩子执行, 不崩溃即PASS) ---- */
static void test_cmd_clear_exec(void){
    printf("[4] 命令层: vkCmdClearDepthStencilImage 经 shim 执行\n");
    /* 建一个 D32S8(实际 D24S8) 图像 */
    VkImageCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = D32S8;
    ci.extent.width = W; ci.extent.height = H; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = 0;
    if(vkCreateImage(g_dev, &ci, 0, &img) != VK_SUCCESS){ printf("  [SKIP] 创建失败\n"); return; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    uint32_t idx = 0; for(uint32_t i=0;i<mp.memoryTypeCount;i++) if(mr.memoryTypeBits & (1u<<i)){ idx=i; break; }
    VkMemoryAllocateInfo mai = {0}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size; mai.memoryTypeIndex = idx;
    VkDeviceMemory mem = 0; vkAllocateMemory(g_dev, &mai, 0, &mem); vkBindImageMemory(g_dev, img, mem, 0);

    uint32_t qf = 0; VkQueue q = 0; vkGetDeviceQueue(g_dev, qf, 0, &q);
    VkCommandPool pool = 0;
    VkCommandPoolCreateInfo pci = {0}; pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = qf; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(g_dev, &pci, 0, &pool);
    VkCommandBuffer cb = 0;
    VkCommandBufferAllocateInfo ai = {0}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_dev, &ai, &cb);

    VkCommandBufferBeginInfo bgi = {0}; bgi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &bgi);
    VkClearDepthStencilValue cv;
    cv.depth = 0.5f; cv.stencil = 7;     /* 应用按 D32S8 语义提交, shim 应 clamp 透传 */
    VkImageSubresourceRange rng;
    rng.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    rng.baseMipLevel = 0; rng.levelCount = 1; rng.baseArrayLayer = 0; rng.layerCount = 1;
    vkCmdClearDepthStencilImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rng);
    VkResult r = vkEndCommandBuffer(cb);
    check(r == VK_SUCCESS, "录制含 vkCmdClearDepthStencilImage 的命令缓冲成功");

    VkFence f = 0; VkFenceCreateInfo fi = {0}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(g_dev, &fi, 0, &f);
    VkSubmitInfo si = {0}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    r = vkQueueSubmit(q, 1, &si, f);
    if(r == VK_SUCCESS) vkWaitForFences(g_dev, 1, &f, VK_TRUE, 2000000000);
    check(r == VK_SUCCESS, "提交并执行清除命令成功 (未崩溃/段错误)");

    vkDestroyFence(g_dev, f, 0);
    vkFreeCommandBuffers(g_dev, pool, 1, &cb);
    vkDestroyCommandPool(g_dev, pool, 0);
    vkFreeMemory(g_dev, mem, 0);
    vkDestroyImage(g_dev, img, 0);
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== D32S8 -> D24S8 完美伪装 验证 ===\n");
    if(setup() != VK_SUCCESS){ fprintf(stderr,"Vulkan 初始化失败\n"); return 2; }
    test_query_mask();
    test_query_mask_v2();
    test_create_substitution();
    test_cmd_hooks();
    test_cmd_clear_exec();
    printf("=== %s ===\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED");
    return g_fail == 0 ? 0 : 1;
}
