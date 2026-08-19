/* probe_d32s8.c — D32_SFLOAT_S8_UINT 全组合 RAW 探针
 * 编译: gcc -O2 -o probe_d32s8 probe_d32s8.c -lvulkan
 * 运行: VK_TEST_RAW=1 VK_ICD_FILENAMES=<json 指向目标 shim> ./probe_d32s8 <combo_index>
 *       每个组合单独一个进程跑, 由脚本 run_probe_d32s8.sh 循环, exit 139 = 驱动段错误。
 *
 * 目的: 查明 D32S8 在哪些组合下真的崩驱动 (现行 shim 全量替换可能过度)。
 * 组合 = (tiling, usage, samples, mipLevels, arrayLayers):
 *   0: OPTIMAL DS_ATTACH                1 1 1   (基线, 已知崩)
 *   1: OPTIMAL DS_ATTACH|SAMPLED        1 1 1
 *   2: OPTIMAL DS_ATTACH                1 1 2   (渲染 layer 0)
 *   3: OPTIMAL DS_ATTACH                1 2 1   (渲染 mip 0)
 *   4: LINEAR  DS_ATTACH                1 1 1
 *   5: OPTIMAL SAMPLED                  1 1 1   (仅建图+视图)
 *   6: OPTIMAL TRANSFER_DST             1 1 1   (仅建图+视图)
 *   7: OPTIMAL DS_ATTACH|TRANSFER_DST   1 1 1
 *   8: OPTIMAL DS_ATTACH                2 1 1   (MSAA, 仅建图+视图)
 *   9: OPTIMAL DS_ATTACH                4 1 1   (MSAA, 仅建图+视图)
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 64
#define H 64
#define CHK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_FAIL %s = %d\n",#x,(int)_r); return _r; } } while(0)

static VkInstance g_inst=0;
static VkPhysicalDevice g_pd=0;
static VkDevice g_dev=0;
static VkQueue g_q=0;
static uint32_t g_qf=0;
static VkCommandPool g_pool=0;
static VkCommandBuffer g_cb=0;

typedef struct { VkImageTiling tiling; VkImageUsageFlags usage; VkSampleCountFlagBits samples; uint32_t mip; uint32_t array; const char* name; } Combo;
static const Combo COMBOS[] = {
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,             VK_SAMPLE_COUNT_1_BIT, 1, 1, "OPTIMAL/DS_ATTACH/1s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT, 1, 1, "OPTIMAL/DS_ATTACH+SAMPLED/1s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,             VK_SAMPLE_COUNT_1_BIT, 1, 2, "OPTIMAL/DS_ATTACH/1s/1m/2a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,             VK_SAMPLE_COUNT_1_BIT, 2, 1, "OPTIMAL/DS_ATTACH/1s/2m/1a" },
    { VK_IMAGE_TILING_LINEAR,  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,             VK_SAMPLE_COUNT_1_BIT, 1, 1, "LINEAR/DS_ATTACH/1s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT,                              VK_SAMPLE_COUNT_1_BIT, 1, 1, "OPTIMAL/SAMPLED/1s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT,                         VK_SAMPLE_COUNT_1_BIT, 1, 1, "OPTIMAL/TRANSFER_DST/1s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, 1, 1, "OPTIMAL/DS_ATTACH+TRANSFER/1s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,             VK_SAMPLE_COUNT_2_BIT, 1, 1, "OPTIMAL/DS_ATTACH/2s/1m/1a" },
    { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,             VK_SAMPLE_COUNT_4_BIT, 1, 1, "OPTIMAL/DS_ATTACH/4s/1m/1a" },
};
#define NCOMBOS (sizeof(COMBOS)/sizeof(COMBOS[0]))
static const VkFormat D32S8 = VK_FORMAT_D32_SFLOAT_S8_UINT;

static uint8_t* readFile(const char* p, size_t* sz){
    FILE* f=fopen(p,"rb"); if(!f){return 0;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t* b=malloc(n); fread(b,1,n,f); fclose(f); *sz=(size_t)n; return b;
}

static VkResult setup(void){
    VkApplicationInfo ai={0}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.pApplicationName="probe_d32s8"; ai.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={0}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&ai;
    CHK(vkCreateInstance(&ici,0,&g_inst));
    uint32_t cnt=0; vkEnumeratePhysicalDevices(g_inst,&cnt,0);
    VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(g_inst,&cnt,pds);
    g_pd=pds[0];
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

/* 用组合参数创建 D32S8 深度图 (可选 color 图用于渲染回读) */
static VkResult makeDepthImage(const Combo* c, VkImage* img, VkDeviceMemory* mem){
    VkImageCreateInfo ci={0}; ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=D32S8;
    ci.extent.width=W; ci.extent.height=H; ci.extent.depth=1; ci.mipLevels=c->mip; ci.arrayLayers=c->array;
    ci.samples=c->samples; ci.tiling=c->tiling; ci.usage=c->usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r=vkCreateImage(g_dev,&ci,0,img);
    if(r!=VK_SUCCESS) return r;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev,*img,&mr);
    VkMemoryAllocateInfo mai={0}; mai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize=mr.size; mai.memoryTypeIndex=0;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd,&mp);
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if(mr.memoryTypeBits&(1u<<i)){ mai.memoryTypeIndex=i; break; }
    r=vkAllocateMemory(g_dev,&mai,0,mem); if(r!=VK_SUCCESS){ vkDestroyImage(g_dev,*img,0); return r; }
    vkBindImageMemory(g_dev,*img,*mem,0);
    return VK_SUCCESS;
}

static VkResult makeColorImage(VkImage* img, VkDeviceMemory* mem){
    VkImageCreateInfo ci={0}; ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent.width=W; ci.extent.height=H; ci.extent.depth=1; ci.mipLevels=1; ci.arrayLayers=1;
    ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkResult r=vkCreateImage(g_dev,&ci,0,img);
    if(r!=VK_SUCCESS) return r;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev,*img,&mr);
    VkMemoryAllocateInfo mai={0}; mai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize=mr.size; mai.memoryTypeIndex=0;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_pd,&mp);
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

/* 渲染 + 回读中心像素。depthImg/depthMem 是已建好的 D32S8 图。返回 0=深度正常(绿) 1=深度失效(蓝) 2=异常 */
static int renderAndReadback(VkImage depthImg){
    VkImage color=0; VkDeviceMemory cm=0;
    if(makeColorImage(&color,&cm)!=VK_SUCCESS){ fprintf(stderr,"color image fail\n"); return 2; }
    VkAttachmentDescription atts[2]={0};
    atts[0].format=VK_FORMAT_R8G8B8A8_UNORM; atts[0].samples=VK_SAMPLE_COUNT_1_BIT; atts[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; atts[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; atts[0].finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    atts[1].format=D32S8; atts[1].samples=VK_SAMPLE_COUNT_1_BIT; atts[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; atts[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; atts[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp={0}; sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount=1; sp.pColorAttachments=&cr; sp.pDepthStencilAttachment=&dr;
    VkRenderPassCreateInfo rpci={0}; rpci.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpci.attachmentCount=2; rpci.pAttachments=atts; rpci.subpassCount=1; rpci.pSubpasses=&sp;
    VkRenderPass rp=0;
    if(vkCreateRenderPass(g_dev,&rpci,0,&rp)!=VK_SUCCESS){ fprintf(stderr,"renderpass fail\n"); vkDestroyImage(g_dev,color,0); vkFreeMemory(g_dev,cm,0); return 2; }
    VkImageView civ=0,div=0;
    VkImageViewCreateInfo civi={0}; civi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; civi.image=color; civi.viewType=VK_IMAGE_VIEW_TYPE_2D; civi.format=VK_FORMAT_R8G8B8A8_UNORM;
    civi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; civi.subresourceRange.levelCount=1; civi.subresourceRange.layerCount=1;
    vkCreateImageView(g_dev,&civi,0,&civ);
    VkImageViewCreateInfo divi={0}; divi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; divi.image=depthImg; divi.viewType=VK_IMAGE_VIEW_TYPE_2D; divi.format=D32S8;
    divi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT; divi.subresourceRange.levelCount=1; divi.subresourceRange.layerCount=1;
    if(vkCreateImageView(g_dev,&divi,0,&div)!=VK_SUCCESS){ fprintf(stderr,"depth view fail\n"); vkDestroyImageView(g_dev,civ,0); vkDestroyRenderPass(g_dev,rp,0); vkDestroyImage(g_dev,color,0); vkFreeMemory(g_dev,cm,0); return 2; }
    VkFramebuffer fb=0;
    VkFramebufferCreateInfo fci={0}; fci.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fci.renderPass=rp; fci.attachmentCount=2; VkImageView av[2]={civ,div}; fci.pAttachments=av; fci.width=W; fci.height=H; fci.layers=1;
    if(vkCreateFramebuffer(g_dev,&fci,0,&fb)!=VK_SUCCESS){ fprintf(stderr,"framebuffer fail\n"); vkDestroyImageView(g_dev,div,0); vkDestroyImageView(g_dev,civ,0); vkDestroyRenderPass(g_dev,rp,0); vkDestroyImage(g_dev,color,0); vkFreeMemory(g_dev,cm,0); return 2; }
    size_t vsz,fsz; uint8_t* vs=readFile("depthtest.vert.spv",&vsz); uint8_t* fs=readFile("depthtest.frag.spv",&fsz);
    if(!vs||!fs){ fprintf(stderr,"missing spv\n"); return 2; }
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
    if(vkCreateGraphicsPipelines(g_dev,0,1,&pci,0,&pipe)!=VK_SUCCESS){ fprintf(stderr,"pipeline fail\n"); return 2; }
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
    vkQueueSubmit(g_q,1,&si,f);
    vkWaitForFences(g_dev,1,&f,VK_TRUE,1000000000);
    vkDestroyFence(g_dev,f,0);
    VkBuffer stb=0; VkDeviceMemory stm=0;
    makeBuffer(W*H*4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &stb,&stm);
    vkResetCommandBuffer(g_cb,0);
    VkCommandBufferBeginInfo bgi2={0}; bgi2.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(g_cb,&bgi2);
    VkBufferImageCopy bic={0}; bic.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; bic.imageSubresource.layerCount=1; bic.imageExtent.width=W; bic.imageExtent.height=H; bic.imageExtent.depth=1;
    vkCmdCopyImageToBuffer(g_cb,color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stb,1,&bic);
    vkEndCommandBuffer(g_cb);
    VkSubmitInfo si2={0}; si2.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si2.commandBufferCount=1; si2.pCommandBuffers=&g_cb;
    VkFence f2=0; vkCreateFence(g_dev,&fi,0,&f2);
    vkQueueSubmit(g_q,1,&si2,f2); vkWaitForFences(g_dev,1,&f2,VK_TRUE,1000000000); vkDestroyFence(g_dev,f2,0);
    void* ptr=0; vkMapMemory(g_dev,stm,0,W*H*4,0,&ptr);
    uint8_t* px=(uint8_t*)ptr + ((H/2)*W + W/2)*4;
    int R=px[0],G=px[1],B=px[2];
    vkUnmapMemory(g_dev,stm);
    vkDestroyBuffer(g_dev,stb,0); vkFreeMemory(g_dev,stm,0);
    vkDestroyPipeline(g_dev,pipe,0); vkDestroyPipelineLayout(g_dev,pl,0);
    vkDestroyShaderModule(g_dev,vmod,0); vkDestroyShaderModule(g_dev,fmod,0);
    vkDestroyImageView(g_dev,div,0); vkDestroyImageView(g_dev,civ,0);
    vkDestroyFramebuffer(g_dev,fb,0); vkDestroyRenderPass(g_dev,rp,0);
    vkDestroyImage(g_dev,color,0); vkFreeMemory(g_dev,cm,0);
    free(vs); free(fs);
    printf("RENDER center RGB=(%d,%d,%d)\n",R,G,B);
    if(G>150 && R<100 && B<100) return 0;   /* 深度正常 */
    if(B>150 && R<100) return 1;            /* 深度失效 */
    return 2;
}

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0); /* 每组合独立进程, 崩溃前也要把结果打出来 */
    int idx=0;
    if(argc>1) idx=atoi(argv[1]);
    if(idx<0 || idx>=(int)NCOMBOS){ fprintf(stderr,"bad combo %d\n",idx); return 2; }
    const Combo* c=&COMBOS[idx];
    if(setup()!=VK_SUCCESS){ return 1; }
    printf("[combo %d] %s\n", idx, c->name);
    printf("  format=%d(130) tiling=%d usage=0x%x samples=%d mip=%u array=%u\n",
           (int)D32S8, (int)c->tiling, (unsigned)c->usage, (int)c->samples, c->mip, c->array);

    /* 1) 建 D32S8 图 */
    VkImage depth=0; VkDeviceMemory dm=0;
    VkResult r=makeDepthImage(c,&depth,&dm);
    if(r!=VK_SUCCESS){ printf("CREATE_IMAGE=FAIL(%d)\n",(int)r); return 0; }
    printf("CREATE_IMAGE=OK\n");

    /* 2) 建视图 */
    VkImageViewCreateInfo divi={0}; divi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; divi.image=depth; divi.viewType=VK_IMAGE_VIEW_TYPE_2D; divi.format=D32S8;
    divi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; divi.subresourceRange.levelCount=1; divi.subresourceRange.layerCount=1;
    VkImageView div=0;
    r=vkCreateImageView(g_dev,&divi,0,&div);
    if(r!=VK_SUCCESS){ printf("CREATE_VIEW=FAIL(%d)\n",(int)r); return 0; }
    printf("CREATE_VIEW=OK\n");

    /* 3) 渲染组合 (仅单采样且含 DS_ATTACH) */
    int renderable = (c->samples==VK_SAMPLE_COUNT_1_BIT) && (c->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if(renderable){
        int res=renderAndReadback(depth);
        printf("DEPTH_RESULT=%s\n", res==0?"GREEN(depth OK)":(res==1?"BLUE(depth broken)":"ABNORMAL"));
    } else {
        printf("RENDER=SKIP (samples=%d usage=0x%x)\n",(int)c->samples,(unsigned)c->usage);
    }
    vkDestroyImageView(g_dev,div,0);
    vkDestroyImage(g_dev,depth,0); vkFreeMemory(g_dev,dm,0);
    return 0;
}
