/* qfmt.c — 直连真实 Adreno HAL, 仅查询 D32S8 vs D24S8 的原始能力 (RAW 模式, 不经 shim 修补)
 * 编译: gcc qfmt.c -o qfmt -lvulkan -I/data/data/com.termux/files/usr/include
 * 运行: VK_TEST_RAW=1 VK_ICD_FILENAMES=$PROJ/.build/vulkan_adreno_icd.json \
 *       LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib ./qfmt
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdint.h>

#define CHK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_FAIL %s=%d\n",#x,(int)_r); return 1; } } while(0)

int main(){
    VkApplicationInfo ai={0}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={0}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&ai;
    VkInstance inst; CHK(vkCreateInstance(&ici,0,&inst));
    uint32_t cnt=0; vkEnumeratePhysicalDevices(inst,&cnt,0);
    if(!cnt){ fprintf(stderr,"no PD\n"); return 1; }
    VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst,&cnt,&pd);
    VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(pd,&prop);
    printf("PhysicalDevice: %s (vendor=0x%x)\n\n", prop.deviceName, prop.vendorID);

    VkFormat fmts[2]={VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    const char* names[2]={"D32_SFLOAT_S8_UINT (=130)", "D24_UNORM_S8_UINT (=129)"};
    for(int k=0;k<2;k++){
        VkFormat f=fmts[k];
        printf("===== %s =====\n", names[k]);
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd,f,&fp);
        printf("FormatProperties.optimalTilingFeatures = 0x%llx\n", (unsigned long long)fp.optimalTilingFeatures);
        printf("  DEPTH_STENCIL_ATTACHMENT = %s\n", (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)?"YES":"no");
        printf("  SAMPLED_IMAGE            = %s\n", (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)?"YES":"no");
        printf("  (raw optimal=0x%llx linear=0x%llx)\n", (unsigned long long)fp.optimalTilingFeatures,(unsigned long long)fp.linearTilingFeatures);

        VkImageFormatProperties ifp;
        VkResult r1=vkGetPhysicalDeviceImageFormatProperties(pd,f,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,0,&ifp);
        printf("ImageFormatProperties(DEPTH_STENCIL_ATTACHMENT) => %s\n", r1==VK_SUCCESS?"VK_SUCCESS (支持)":"VK_ERROR_FORMAT_NOT_SUPPORTED (不支持)");
        VkResult r2=vkGetPhysicalDeviceImageFormatProperties(pd,f,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&ifp);
        printf("ImageFormatProperties(DEPTH_ATTACHMENT|SAMPLED) => %s\n", r2==VK_SUCCESS?"VK_SUCCESS (支持)":"VK_ERROR_FORMAT_NOT_SUPPORTED (不支持)");
        printf("\n");
    }
    return 0;
}
