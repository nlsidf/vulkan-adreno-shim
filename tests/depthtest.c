/* depthtest.c — 直连 Adreno 真实 HAL, 验证 D32S8 vs D24S8 深度支持
 * 编译: gcc depthtest.c -o depthtest -lvulkan -I/data/data/com.termux/files/usr/include
 * 运行: VK_TEST_RAW=1 VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json \
 *       LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib ./depthtest
 *
 * 做法: 对每种深度格式
 *   1) 查询原始 FormatProperties / ImageFormatProperties (不经 shim 修补)
 *   2) 真实渲染一次深度测试: 先画"近"绿三角(z=0.3), 再画"远"蓝三角(z=0.7),
 *      两三角覆盖同一区域, 深度测试 LESS。
 *      - 深度正常 => 蓝被丢弃, 中心像素=绿
 *      - 深度失效(后画覆盖) => 中心像素=蓝
 *      - 若创建/渲染报错 => 该格式"不可用"
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 64
#define H 64
#define CHK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_FAIL %s = %d @%s:%d\n",#x,(int)_r,__FILE__,__LINE__); return _r; } } while(0)

static VkInstance g_inst=0;
static VkPhysicalDevice g_pd=0;
static VkDevice g_dev=0;
static VkQueue g_q=0;
static uint32_t g_qf=0;
static VkCommandPool g_pool=0;
static VkCommandBuffer g_cb=0;

static uint8_t* readFile(const char* p, size_t* sz){
    FILE* f=fopen(p,"rb"); if(!f){return 0;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t* b=malloc(n); fread(b,1,n,f); fclose(f); *sz=(size_t)n; return b;
}

static VkResult setup(const char* drvname){
    VkApplicationInfo ai={0}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.pApplicationName="depthtest"; ai.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={0}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&ai;
    CHK(vkCreateInstance(&ici,0,&g_inst));
    uint32_t cnt=0; vkEnumeratePhysicalDevices(g_inst,&cnt,0);
    if(!cnt){ fprintf(stderr,"no PD\n"); return VK_ERROR_UNKNOWN; }
    VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(g_inst,&cnt,pds);
    g_pd=pds[0];
    VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(g_pd,&prop);
    printf("=== PhysicalDevice: %s (vendor=0x%x) ===\n", prop.deviceName, prop.vendorID);
    // 选图形队列
    uint32_t qc=0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd,&qc,0);
    VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(g_pd,&qc,qp);
    for(uint32_t i=0;i<qc;i++) if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){ g_qf=i; break; }
    float qp1=1.0f;
    VkDeviceQueueCreateInfo dqci={0}; dqci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; dqci.queueFamilyIndex=g_qf; dqci.queueCount=1; dqci.pQueuePriorities=&qp1;
    VkDeviceCreateInfo dci={0}; dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&dqci;
    CHK(vkCreateDevice(g_pd,&dci,0,&g_dev));
    vkGetDeviceQueue(g_dev,g_qf,0,&g_q);
    VkCommandPoolCreateInfo pci={0}; pci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.queueFamilyIndex=g_qf; pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CHK(vkCreateCommandPool(g_dev,&pci,0,&g_pool));
    VkCommandBufferAllocateInfo ai2={0}; ai2.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai2.commandPool=g_pool; ai2.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai2.commandBufferCount=1;
    CHK(vkAllocateCommandBuffers(g_dev,&ai2,&g_cb));
    return VK_SUCCESS;
}

static VkResult makeImage(VkFormat fmt, VkImageUsageFlags usage, VkImage* img, VkDeviceMemory* mem){
    VkImageCreateInfo ci={0}; ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=fmt;
    ci.extent.width=W; ci.extent.height=H; ci.extent.depth=1; ci.mipLevels=1; ci.arrayLayers=1;
    ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r=vkCreateImage(g_dev,&ci,0,img);
    if(r!=VK_SUCCESS) return r;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev,*img,&mr);
    VkMemoryAllocateInfo mai={0}; mai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize=mr.size;
    mai.memoryTypeIndex=0; // 先用 0; 下面修正
    // 选 DEVICE_LOCAL 类型
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd,&mp);
    mai.memoryTypeIndex=mp.memoryTypeCount; // 哨兵
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if(mr.memoryTypeBits&(1u<<i)){ mai.memoryTypeIndex=i; break; }
    r=vkAllocateMemory(g_dev,&mai,0,mem); if(r!=VK_SUCCESS){ vkDestroyImage(g_dev,*img,0); return r; }
    vkBindImageMemory(g_dev,*img,*mem,0);
    return VK_SUCCESS;
}

static VkResult makeBuffer(VkDeviceSize sz, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem){
    VkBufferCreateInfo ci={0}; ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ci.size=sz; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    CHK(vkCreateBuffer(g_dev,&ci,0,buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev,*buf,&mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd,&mp);
    uint32_t idx=mp.memoryTypeCount;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++){ if((mr.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){ idx=i; break; } }
    VkMemoryAllocateInfo mai={0}; mai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize=mr.size; mai.memoryTypeIndex=idx;
    CHK(vkAllocateMemory(g_dev,&mai,0,mem));
    CHK(vkBindBufferMemory(g_dev,*buf,*mem,0));
    return VK_SUCCESS;
}

typedef struct { float color[4]; float z; float pad[3]; } PC;
/* PC 应为 32 字节: vec4 color(16) + float z(4) + pad(12) */

/* 对一种深度格式跑一次, 返回中心像素 rgb (0..255) 与结果码 */
static VkResult runDepth(VkFormat depthFmt, int* outR, int* outG, int* outB, int* createFailed){
    *createFailed=0;
    VkImage color=0, depth=0; VkDeviceMemory cm=0,dm=0;
    VkResult r=makeImage(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &color,&cm);
    if(r!=VK_SUCCESS){ *createFailed=1; return r; }
    r=makeImage(depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth,&dm);
    if(r!=VK_SUCCESS){ *createFailed=1; vkDestroyImage(g_dev,color,0); vkFreeMemory(g_dev,cm,0); return r; }

    VkAttachmentDescription atts[2]={0};
    atts[0].format=VK_FORMAT_R8G8B8A8_UNORM; atts[0].samples=VK_SAMPLE_COUNT_1_BIT; atts[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; atts[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; atts[0].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    atts[1].format=depthFmt; atts[1].samples=VK_SAMPLE_COUNT_1_BIT; atts[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; atts[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; atts[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp={0}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount=1; sp.pColorAttachments=&cr; sp.pDepthStencilAttachment=&dr;
    VkRenderPassCreateInfo rpci={0}; rpci.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpci.attachmentCount=2; rpci.pAttachments=atts; rpci.subpassCount=1; rpci.pSubpasses=&sp;
    VkRenderPass rp=0;
    r=vkCreateRenderPass(g_dev,&rpci,0,&rp);
    if(r!=VK_SUCCESS){ *createFailed=1; vkDestroyImage(g_dev,color,0); vkDestroyImage(g_dev,depth,0); vkFreeMemory(g_dev,cm,0); vkFreeMemory(g_dev,dm,0); return r; }

    VkImageView civ=0,div=0;
    VkImageViewCreateInfo civi={0}; civi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; civi.image=color; civi.viewType=VK_IMAGE_VIEW_TYPE_2D; civi.format=VK_FORMAT_R8G8B8A8_UNORM;
    civi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; civi.subresourceRange.levelCount=1; civi.subresourceRange.layerCount=1;
    vkCreateImageView(g_dev,&civi,0,&civ);
    VkImageViewCreateInfo divi={0}; divi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; divi.image=depth; divi.viewType=VK_IMAGE_VIEW_TYPE_2D; divi.format=depthFmt;
    divi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT; divi.subresourceRange.levelCount=1; divi.subresourceRange.layerCount=1;
    r=vkCreateImageView(g_dev,&divi,0,&div);
    if(r!=VK_SUCCESS){ *createFailed=1; }

    VkFramebuffer fb=0;
    if(r==VK_SUCCESS){
        VkFramebufferCreateInfo fci={0}; fci.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fci.renderPass=rp; fci.attachmentCount=2; VkImageView av[2]={civ,div}; fci.pAttachments=av; fci.width=W; fci.height=H; fci.layers=1;
        r=vkCreateFramebuffer(g_dev,&fci,0,&fb);
        if(r!=VK_SUCCESS) *createFailed=1;
    }

    // shader
    size_t vsz,fsz; uint8_t* vs=readFile("depthtest.vert.spv",&vsz); uint8_t* fs=readFile("depthtest.frag.spv",&fsz);
    if(!vs||!fs){ fprintf(stderr,"缺少 spv\n"); return VK_ERROR_UNKNOWN; }
    VkShaderModuleCreateInfo smi={0}; smi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; smi.codeSize=vsz; smi.pCode=(uint32_t*)vs;
    VkShaderModule vmod=0; vkCreateShaderModule(g_dev,&smi,0,&vmod);
    smi.codeSize=fsz; smi.pCode=(uint32_t*)fs; VkShaderModule fmod=0; vkCreateShaderModule(g_dev,&smi,0,&fmod);

    VkPipelineLayout pl=0; VkPipelineLayoutCreateInfo plci={0}; plci.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPushConstantRange pcr={0}; pcr.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset=0; pcr.size=sizeof(PC);
    plci.pushConstantRangeCount=1; plci.pPushConstantRanges=&pcr; vkCreatePipelineLayout(g_dev,&plci,0,&pl);

    VkPipelineShaderStageCreateInfo ss[2]={0};
    ss[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ss[0].stage=VK_SHADER_STAGE_VERTEX_BIT; ss[0].module=vmod; ss[0].pName="main";
    ss[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ss[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module=fmod; ss[1].pName="main";
    VkPipelineVertexInputStateCreateInfo vis={0}; vis.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ias={0}; ias.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ias.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps={0}; vps.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    VkViewport vp={0,0,(float)W,(float)H,0,1}; VkRect2D sc={{0,0},W,H}; vps.viewportCount=1; vps.pViewports=&vp; vps.scissorCount=1; vps.pScissors=&sc;
    VkPipelineRasterizationStateCreateInfo rs={0}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.lineWidth=1.0f;
    VkPipelineMultisampleStateCreateInfo ms={0}; ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds={0}; ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba={0}; ba.colorWriteMask=0xf;
    VkPipelineColorBlendStateCreateInfo cb={0}; cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount=1; cb.pAttachments=&ba;
    VkGraphicsPipelineCreateInfo pci={0}; pci.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; pci.stageCount=2; pci.pStages=ss; pci.pVertexInputState=&vis; pci.pInputAssemblyState=&ias; pci.pViewportState=&vps; pci.pRasterizationState=&rs; pci.pMultisampleState=&ms; pci.pDepthStencilState=&ds; pci.pColorBlendState=&cb; pci.layout=pl; pci.renderPass=rp; pci.subpass=0;
    VkPipeline pipe=0;
    if(!*createFailed){ r=vkCreateGraphicsPipelines(g_dev,0,1,&pci,0,&pipe); if(r!=VK_SUCCESS){ *createFailed=1; } }

    // 渲染
    if(!*createFailed){
        vkResetCommandBuffer(g_cb,0);
        VkCommandBufferBeginInfo bgi={0}; bgi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(g_cb,&bgi);
        VkClearValue cv[2];
        cv[0].color.float32[0]=1.0f; cv[0].color.float32[1]=0.0f; cv[0].color.float32[2]=0.0f; cv[0].color.float32[3]=1.0f;
        cv[1].depthStencil.depth=1.0f; cv[1].depthStencil.stencil=0;
        VkRenderPassBeginInfo rb={0}; rb.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass=rp; rb.framebuffer=fb; rb.renderArea.extent.width=W; rb.renderArea.extent.height=H; rb.clearValueCount=2; rb.pClearValues=cv;
        vkCmdBeginRenderPass(g_cb,&rb,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(g_cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
        PC pc_near={ .color={0,1,0,1}, .z=0.3f };
        PC pc_far ={ .color={0,0,1,1}, .z=0.7f };
        vkCmdPushConstants(g_cb,pl,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PC),&pc_near);
        vkCmdDraw(g_cb,3,1,0,0);
        vkCmdPushConstants(g_cb,pl,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(PC),&pc_far);
        vkCmdDraw(g_cb,3,1,0,0);
        vkCmdEndRenderPass(g_cb);
        vkEndCommandBuffer(g_cb);
        VkSubmitInfo si={0}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&g_cb;
        VkFence f=0; VkFenceCreateInfo fi={0}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(g_dev,&fi,0,&f);
        r=vkQueueSubmit(g_q,1,&si,f);
        vkWaitForFences(g_dev,1,&f,VK_TRUE,1000000000);
        vkDestroyFence(g_dev,f,0);
        if(r!=VK_SUCCESS){ *createFailed=1; }
    }

    // 读回
    if(!*createFailed){
        VkBuffer stb=0; VkDeviceMemory stm=0;
        makeBuffer(W*H*4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &stb,&stm);
        vkResetCommandBuffer(g_cb,0);
        VkCommandBufferBeginInfo bgi={0}; bgi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(g_cb,&bgi);
        VkImageMemoryBarrier imb={0}; imb.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; imb.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; imb.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; imb.image=color; imb.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; imb.subresourceRange.levelCount=1; imb.subresourceRange.layerCount=1;
        // 直接拷贝 (final layout 已是 TRANSFER_SRC_OPTIMAL)
        VkBufferImageCopy bic={0}; bic.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; bic.imageSubresource.layerCount=1; bic.imageExtent.width=W; bic.imageExtent.height=H; bic.imageExtent.depth=1;
        vkCmdCopyImageToBuffer(g_cb,color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stb,1,&bic);
        vkEndCommandBuffer(g_cb);
        VkSubmitInfo si={0}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&g_cb;
        VkFence f=0; VkFenceCreateInfo fi={0}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; vkCreateFence(g_dev,&fi,0,&f);
        vkQueueSubmit(g_q,1,&si,f); vkWaitForFences(g_dev,1,&f,VK_TRUE,1000000000); vkDestroyFence(g_dev,f,0);
        void* ptr=0; vkMapMemory(g_dev,stm,0,W*H*4,0,&ptr);
        uint8_t* px=(uint8_t*)ptr + ((H/2)*W + W/2)*4;
        *outR=px[0]; *outG=px[1]; *outB=px[2];
        vkUnmapMemory(g_dev,stm);
        vkDestroyBuffer(g_dev,stb,0); vkFreeMemory(g_dev,stm,0);
    }

    if(pipe) vkDestroyPipeline(g_dev,pipe,0);
    if(pl) vkDestroyPipelineLayout(g_dev,pl,0);
    if(vmod) vkDestroyShaderModule(g_dev,vmod,0);
    if(fmod) vkDestroyShaderModule(g_dev,fmod,0);
    if(civ) vkDestroyImageView(g_dev,civ,0);
    if(div) vkDestroyImageView(g_dev,div,0);
    if(fb) vkDestroyFramebuffer(g_dev,fb,0);
    if(rp) vkDestroyRenderPass(g_dev,rp,0);
    vkDestroyImage(g_dev,color,0); vkDestroyImage(g_dev,depth,0);
    vkFreeMemory(g_dev,cm,0); vkFreeMemory(g_dev,dm,0);
    free(vs); free(fs);
    return *createFailed ? r : VK_SUCCESS;
}

static const char* fmtName(VkFormat f){ return (f==VK_FORMAT_D32_SFLOAT_S8_UINT)?"D32_SFLOAT_S8_UINT(130)":"D24_UNORM_S8_UINT(129)"; }

int main(int argc,char**argv){
    if(setup(0)!=VK_SUCCESS){ return 1; }
    VkFormat fmts[2]={VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    int lo=0, hi=1;
    if(argc>1){ int idx=atoi(argv[1]); lo=idx; hi=idx; }
    for(int k=lo;k<=hi;k++){
        VkFormat f=fmts[k];
        printf("\n########## 测试深度格式: %s ##########\n", fmtName(f));
        // 1) 原始能力查询
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd,f,&fp);
        printf("[FormatProperties] optimal=0x%llx linear=0x%llx  => DEPTH_ATTACH=0x%llx SAMPLED=0x%llx\n",
               (unsigned long long)fp.optimalTilingFeatures,(unsigned long long)fp.linearTilingFeatures,
               (unsigned long long)(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
               (unsigned long long)(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
        VkImageFormatProperties ifp; VkResult ir=vkGetPhysicalDeviceImageFormatProperties(g_pd,f,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,0,&ifp);
        printf("[ImageFormatProperties] 用作 DEPTH_ATTACHMENT => %s\n", ir==VK_SUCCESS?"VK_SUCCESS (支持)":"VK_ERROR_FORMAT_NOT_SUPPORTED (不支持)");

        // 2) 真实渲染
        int R,G,B,failed=0;
        VkResult rr=runDepth(f,&R,&G,&B,&failed);
        if(failed){
            printf("[渲染] 创建/渲染失败 (vkResult=%d) => 该格式不可用\n", (int)rr);
            printf(">>> 结论: %s 在 Adreno 540 上【不可用】\n", fmtName(f));
        } else {
            printf("[渲染] 中心像素 RGB=(%d,%d,%d)\n", R,G,B);
            if(G>150 && R<100 && B<100) printf(">>> 结论: 深度测试【正常】(远蓝三角被近绿三角遮挡) => %s 可用\n", fmtName(f));
            else if(B>150 && R<100) printf(">>> 结论: 深度测试【失效/后画覆盖】(看到远蓝三角) => %s 渲染不正确\n", fmtName(f));
            else printf(">>> 结论: 结果异常 RGB=(%d,%d,%d), %s 行为可疑\n", R,G,B, fmtName(f));
        }
    }
    return 0;
}
