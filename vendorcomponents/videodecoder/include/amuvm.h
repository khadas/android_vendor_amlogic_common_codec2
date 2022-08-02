#ifndef AMUVM_H
#define AMUVM_H

#define AMUVM_DEV "/dev/uvm"
#define UVM_IMM_ALLOC	(1 << 0)
#define UVM_DELAY_ALLOC	(1 << 1)
#define UVM_USAGE_PROTECTED (1 << 3)
#define META_DATA_SIZE 256

#define UVM_META_DATA_VF_BASE_INFOS (1 << 0)
#define UVM_META_DATA_HDR10P_DATA (1 << 1)

#include <stdint.h>
#define u8 unsigned char

#define CODEC_MODE(a, b, c, d)\
	(((u8)(a) << 24) | ((u8)(b) << 16) | ((u8)(c) << 8) | (u8)(d))

#define META_DATA_MAGIC CODEC_MODE('M', 'E', 'T', 'A')

#define UVM_IOC_MAGIC 'U'
#define UVM_IOC_ALLOC _IOWR((UVM_IOC_MAGIC), 0, \
        struct uvm_alloc_data)
#define UVM_IOC_FREE _IOWR(UVM_IOC_MAGIC, 1, \
        struct uvm_alloc_data)
#define UVM_IOC_SET_PID _IOWR(UVM_IOC_MAGIC, 2, \
        struct uvm_pid_data)
#define UVM_IOC_SET_FD _IOWR(UVM_IOC_MAGIC, 3, \
        struct uvm_fd_data)
#define UVM_IOC_GET_METADATA _IOWR(UVM_IOC_MAGIC, 4, \
        struct uvm_meta_data)

struct uvm_alloc_data {
    int size;
    int align;
    unsigned int flags;
    int v4l2_fd;
    int fd;
    int byte_stride;
    uint32_t width;
    uint32_t height;
    int scalar;
    int scaled_buf_size;
};

struct uvm_meta_data {
    int fd;
    int type;
    int size;
    uint8_t data[META_DATA_SIZE];
};

#define AML_META_HEAD_NUM  (8)
#define AML_META_HEAD_SIZE (AML_META_HEAD_NUM * sizeof(uint32_t))

struct aml_meta_head_s {
    uint32_t magic;
    uint32_t type;
    uint32_t data_size;
    uint32_t data[5];
};

struct aml_vf_base_info_s {
    uint32_t width;
    uint32_t height;
    uint32_t duration;
    uint32_t frame_type;
    uint32_t type;
    uint32_t data[12];
};

struct aml_meta_info_s {
    union {
        struct aml_meta_head_s head;
        uint32_t buf[AML_META_HEAD_NUM];
    };
    unsigned char  data[0];
};

int amuvm_open();
int amuvm_close(int uvmfd);
int amuvm_allocate(int uvmfd, int size, int width, int height, unsigned int flag, int *sharefd);
int amuvm_getmetadata(int uvmfd, int fd, unsigned char * meta);
int amuvm_free(int sharefd);

#endif