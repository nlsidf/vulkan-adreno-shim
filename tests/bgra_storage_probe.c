#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef void* (*getip_t)(void*,const char*);
typedef int (*ci_t)(const void*,const void*,void**);
typedef int (*epd_t)(void*,uint32_t*,void**);
typedef int (*iffp_t)(void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,void*);
int main(void){
  void* h=dlopen(getenv("SHIM")?getenv("SHIM"):"/data/data/com.termux/files/home/proton11/.build/vulkan_adreno_icd.so",RTLD_NOW);
  getip_t g=(getip_t)dlsym(h,"vk_icdGetInstanceProcAddr");
  ci_t C=(ci_t)g(NULL,"vkCreateInstance"); epd_t E=(epd_t)g(NULL,"vkEnumeratePhysicalDevices");
  void* inst=NULL; C(&(struct{uint32_t s;const void*n;uint32_t f;const void*ai;uint32_t lc;const char**pl;uint32_t ec;const char**pe;}){ .s=7 },NULL,&inst);
  void* pd=NULL; uint32_t n=1; E(inst,&n,&pd);
  iffp_t F=(iffp_t)g(inst,"vkGetPhysicalDeviceImageFormatProperties");
  // B8G8R8A8 = 44. usage SAMPLED(4)|TRANSFER_SRC(1)|TRANSFER_DST(2)|STORAGE(8)=0xF
  struct{uint32_t me[3];uint32_t mip;uint32_t ly;uint32_t sc;uint32_t rs;} p={0};
  int r=F(pd,44,1,0,0xF,0,&p);
  printf("B8G8R8A8 OPTIMAL usage=SAMPLED|TRANSFER|STORAGE -> r=%d (0=OK,-7=NOT_SUPPORTED)\n",r);
  struct{uint32_t me[3];uint32_t mip;uint32_t ly;uint32_t sc;uint32_t rs;} p2={0};
  int r2=F(pd,44,1,0,0x4|0x1|0x2,0,&p2); // without STORAGE
  printf("B8G8R8A8 OPTIMAL usage=SAMPLED|TRANSFER (no STORAGE) -> r=%d\n",r2);
  return 0;
}
