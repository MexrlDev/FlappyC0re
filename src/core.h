/* SPDX-License-Identifier: MIT */
#ifndef CORE_H
#define CORE_H

typedef __UINT8_TYPE__   u8;
typedef __UINT16_TYPE__  u16;
typedef __UINT32_TYPE__  u32;
typedef __UINT64_TYPE__  u64;

typedef __INT8_TYPE__    s8;
typedef __INT16_TYPE__   s16;
typedef __INT32_TYPE__   s32;
typedef __INT64_TYPE__   s64;

typedef __SIZE_TYPE__    usize;
typedef __PTRDIFF_TYPE__ isize;
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ssize_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifndef NULL
#  define NULL ((void *)0)
#endif

#define HOST_GATE_OFF        0x31AA9u
#define HOST_KERNEL_HANDLE   0x2001
#define HOST_OFF_GS_THREAD   0x057F89B0u
#define HOST_OFF_VIDEO_OUT   0x02d695d0u
#define HOST_OFF_SAVEDATA    0x003893F0u

#define SCREEN_W   1920
#define SCREEN_H   1080

#define FRAME_BYTES   (SCREEN_W * SCREEN_H * 4)
#define FRAME_STRIDE  ((FRAME_BYTES + 0x1FFFFF) & ~0x1FFFFF)
#define FRAME_TOTAL   (FRAME_STRIDE * 2)

#define AUDIO_RATE      48000u
#define AUDIO_FMT_S16   1

__attribute__((naked, noinline))
static u64 nc_call_gate(void *gate, void *target,
                        u64 a0, u64 a1, u64 a2,
                        u64 a3, u64 a4, u64 a5)
{
    __asm__ volatile(
        "pushq  %%rbx\n\t"
        "movq   %%rsi, %%rbx\n\t"
        "movq   %%rdi, %%rax\n\t"
        "movq   %%rdx, %%rdi\n\t"
        "movq   %%rcx, %%rsi\n\t"
        "movq   %%r8,  %%rdx\n\t"
        "movq   %%r9,  %%rcx\n\t"
        "movq   16(%%rsp), %%r8\n\t"
        "movq   24(%%rsp), %%r9\n\t"
        "callq  *%%rax\n\t"
        "popq   %%rbx\n\t"
        "retq"
        ::: "memory"
    );
}

__attribute__((unused))
static void *nc_sym_lookup(void *gate, void *dlsym,
                           s32 module, const char *name)
{
    void *out = NULL;
    nc_call_gate(gate, dlsym,
                 (u64)(s64)module,
                 (u64)name,
                 (u64)&out,
                 0, 0, 0);
    return out;
}

#define NC             nc_call_gate
#define SYM            nc_sym_lookup
#define native_call    nc_call_gate
#define resolve_sym    nc_sym_lookup

#define GADGET_OFFSET     HOST_GATE_OFF
#define LIBKERNEL_HANDLE  HOST_KERNEL_HANDLE
#define EBOOT_GS_THREAD   HOST_OFF_GS_THREAD
#define EBOOT_VIDOUT      HOST_OFF_VIDEO_OUT
#define EBOOT_SAVEDATA_MOUNT_GOT  HOST_OFF_SAVEDATA

#define SCR_W             SCREEN_W
#define SCR_H             SCREEN_H
#define FB_SIZE           FRAME_BYTES
#define FB_ALIGNED        FRAME_STRIDE
#define FB_TOTAL          FRAME_TOTAL
#define SAMPLE_RATE       AUDIO_RATE
#define AUDIO_S16_STEREO  AUDIO_FMT_S16

#define PERSIST  __attribute__((section(".ps_persist")))

#endif /* CORE_H */
