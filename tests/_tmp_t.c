#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
int main(){ void* h=dlopen("/system/lib64/libvulkan.so",RTLD_NOW); printf("dlopen libvulkan=%p %s\n",(void*)h,h? "OK":"FAIL"); return 0;}
