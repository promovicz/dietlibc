#ifndef _ASM_TYPES_H
#define _ASM_TYPES_H

#include <sys/types.h>

#ifdef __alpha__
typedef unsigned int umode_t;
#else
typedef unsigned short umode_t;
#endif

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;

typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;

typedef size_t __kernel_size_t;

#endif