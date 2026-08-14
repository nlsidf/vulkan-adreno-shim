/* qfmt_color.c — enumerate raw Adreno 540 HAL color-format capabilities.
 * Identifies the "lied set": formats the HAL reports as SAMPLED_IMAGE but NOT
 * COLOR_ATTACHMENT, which the ICD shim (vulkan_adreno_icd.c fmtp_fix_depth)
 * then promotes to COLOR_ATTACHMENT (a guess). If the game's 3D offscreen
 * render-target format falls in this set, the HW cannot actually render to it
 * -> black framebuffer.
 *
 * Run RAW (no shim fix) to see true HW behaviour:
 *   export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
 *   export VK_ICD_FILENAMES=$HOME/proton11/.build/vulkan_adreno_icd.json
 *   export VK_TEST_RAW=1
 *   ./qfmt_color
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdint.h>

#define CHK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ fprintf(stderr,"VK_FAIL %s=%d\n",#x,(int)_r); return 1; } } while(0)

/* Known RT candidate format IDs (Vulkan 1.0 VkFormat enum values). */
static const char* fmtname(int f){
    switch(f){
        case 37: return "R8G8B8A8_UNORM";
        case 38: return "R8G8B8A8_SNORM";
        case 43: return "R8G8B8A8_SRGB";
        case 44: return "B8G8R8A8_UNORM";
        case 45: return "B8G8R8A8_SNORM";
        case 50: return "B8G8R8A8_SRGB";
        case 51: return "A8B8G8R8_UNORM_PACK32";
        case 57: return "A8B8G8R8_SRGB_PACK32";
        case 90: return "R16G16B16_UNORM";
        case 91: return "R16G16B16_SNORM";
        case 92: return "R16G16B16_SFLOAT";
        case 93: return "R32G32B32_SFLOAT";
        case 94: return "R32G32B32A32_SFLOAT";
        case 95: return "R10G10B10A2_UNORM";
        case 96: return "R10G10B10A2_SNORM";
        case 97: return "R11G11B10_FLOAT";
        case 98: return "R16G16B16A16_UNORM";
        case 99: return "R16G16B16A16_SNORM";
        case 100: return "R16G16B16A16_SFLOAT";
        case 101: return "R32G32B32A32_SFLOAT";
        case 102: return "R64G64_SFLOAT";
        case 103: return "R64G64B64_SFLOAT";
        case 104: return "R64G64B64A64_SFLOAT";
        case 105: return "B10G11R11_UFLOAT_PACK32";
        case 106: return "R16G16_SFLOAT";
        default: return NULL;
    }
}

int main(){
    VkApplicationInfo ai={0}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici={0}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&ai;
    VkInstance inst; CHK(vkCreateInstance(&ici,0,&inst));
    uint32_t cnt=0; vkEnumeratePhysicalDevices(inst,&cnt,0);
    if(!cnt){ fprintf(stderr,"no PD\n"); return 1; }
    VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst,&cnt,&pd);
    VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(pd,&prop);
    printf("PhysicalDevice: %s (vendor=0x%x)  [RAW HAL caps]\n\n", prop.deviceName, prop.vendorID);

    const VkFormatFeatureFlags CA = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    const VkFormatFeatureFlags SA = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    const VkFormatFeatureFlags DSA = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

    printf("=== LIE-CANDIDATE color formats (HAL: SAMPLED && !COLOR_ATTACHMENT) ===\n");
    int lie=0;
    for (int f=1; f<=200; f++){
        VkFormat fmt=(VkFormat)f;
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd,fmt,&fp);
        VkFormatFeatureFlags opt=fp.optimalTilingFeatures;
        if ((opt & SA) && !(opt & CA) && !(opt & DSA)){
            const char* n=fmtname((int)fmt);
            printf("  fmt=%-3d (%s) opt=0x%016llx lin=0x%016llx\n",
                   f, n?n:"?", (unsigned long long)opt, (unsigned long long)fp.linearTilingFeatures);
            lie++;
        }
    }
    printf("=== total LIE-CANDIDATE formats: %d ===\n\n", lie);

    printf("=== color formats HAL genuinely supports as COLOR_ATTACHMENT ===\n");
    int ca=0;
    for (int f=1; f<=200; f++){
        VkFormat fmt=(VkFormat)f;
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd,fmt,&fp);
        if (fp.optimalTilingFeatures & CA){
            const char* n=fmtname((int)fmt);
            printf("  fmt=%-3d (%s) opt=0x%016llx\n", f, n?n:"?", (unsigned long long)fp.optimalTilingFeatures);
            ca++;
        }
    }
    printf("=== total COLOR_ATTACHMENT-capable formats: %d ===\n", ca);
    return 0;
}
