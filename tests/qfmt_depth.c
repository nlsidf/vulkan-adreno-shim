/* qfmt_depth.c — probe raw Adreno 540 HAL support for depth/stencil formats.
 * Checks both vkGetPhysicalDeviceFormatProperties (DEPTH_STENCIL_ATTACHMENT bit)
 * and vkGetPhysicalDeviceImageFormatProperties (DEPTH_STENCIL_ATTACHMENT usage).
 * Run RAW (VK_TEST_RAW=1) to bypass shim fixes:
 *   export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
 *   export VK_ICD_FILENAMES=$HOME/proton11/.build/vulkan_adreno_icd.json
 *   export VK_TEST_RAW=1
 *   ./qfmt_depth
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
    printf("PhysicalDevice: %s  [RAW HAL depth caps]\n\n", prop.deviceName);

    VkFormat depths[7]={
        VK_FORMAT_D16_UNORM,                 // 124
        VK_FORMAT_X8_D24_UNORM_PACK32,       // 125
        VK_FORMAT_D32_SFLOAT,                // 126
        VK_FORMAT_S8_UINT,                   // 127
        VK_FORMAT_D16_UNORM_S8_UINT,         // 128
        VK_FORMAT_D24_UNORM_S8_UINT,         // 129
        VK_FORMAT_D32_SFLOAT_S8_UINT,        // 130
    };
    const char* names[7]={"D16_UNORM(124)","X8_D24(125)","D32F(126)","S8(127)",
                          "D16S8(128)","D24S8(129)","D32S8(130)"};
    for(int k=0;k<7;k++){
        VkFormat f=depths[k];
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd,f,&fp);
        int dsa = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)?1:0;
        VkImageFormatProperties ifp;
        VkResult r=vkGetPhysicalDeviceImageFormatProperties(pd,f,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,0,&ifp);
        printf("%-14s optDSA=%d  iffp(DS_ATTACH)=%s\n", names[k], dsa,
               r==VK_SUCCESS?"VK_SUCCESS":"FORMAT_NOT_SUPPORTED");
    }
    return 0;
}
