 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>

 #include <sys/types.h>
 #include <sys/stat.h>
 #include <errno.h>
 #include <sys/ioctl.h>
 #include <fcntl.h>
 #include <sys/mman.h>
 #include <errno.h>
 #include <sys/times.h>

 #include "amuvm.h"

 #define UVM_DEBUG

 #ifdef UVM_DEBUG
 #ifdef ANDROID
 #include <android/log.h>
 #include <stdio.h>
 #include <stdarg.h>
 #include <string.h>
 #define  LOG_TAG    "amuvm"
 #define UVM_PRINT(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
 #else
 #define UVM_PRINT(f,s...) fprintf(stderr,f,##s)
 #endif
 #else
 #define UVM_PRINT(f,s...)
 #endif

#define AMUVM_DEV "/dev/uvm"

int amuvm_open() {
    int uvm_fd = open("/dev/uvm", O_RDONLY | O_CLOEXEC);
    if (uvm_fd < 0) {
        UVM_PRINT("open uvm dev fail");
        return -1;
    }

    return uvm_fd;
}
int amuvm_close(int uvmfd) {
    if (uvmfd > 0)
        close(uvmfd);

    return 0;
}

int amuvm_allocate(int uvmfd, int size, int width, int height, unsigned int flag, int *sharefd) {
    if (uvmfd < 0) {
        UVM_PRINT("need open uvm first");
        return -1;
    }
struct uvm_alloc_data uad = {
        .size = size,
        .byte_stride = width,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .align = 0,
        .flags = flag,
        .scalar = 1,
    };

    int ret = ioctl(uvmfd, UVM_IOC_ALLOC, &uad);
    if (ret < 0) {
        UVM_PRINT("uvm alloc error ret=%x", ret);
        return -1;
    }
    *sharefd = uad.fd;

    return 0;
}

int amuvm_getmetadata(int uvmfd, int fd, unsigned char * meta) {
    if (uvmfd < 0 || fd < 0 || meta == NULL) {
        UVM_PRINT("uvm get metadata error, invalid arguments!");
        return -1;
    }

    struct uvm_meta_data uad = {
        .fd = fd,
    };
    int ret = ioctl(uvmfd, UVM_IOC_GET_METADATA, &uad);
    if (ret < 0) {
        UVM_PRINT("uvm get metadata error ret=%x", ret);
        return -1;
    }

    if (uad.size <= 0) {
        return -1;
    }
    memcpy(meta, uad.data, uad.size);

    return uad.size;
}

int amuvm_free(int sharefd) {
    if (sharefd >= 0) {
        close(sharefd);
    }

    return 0;
}