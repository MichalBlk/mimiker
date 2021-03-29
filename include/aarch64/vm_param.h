#ifndef _AARCH64_VM_PARAM_H_
#define _AARCH64_VM_PARAM_H_

#define PAGESIZE 4096
#define SUPERPAGESIZE (1 << 21) /* 2 MB */

#define KERNEL_SPACE_BEGIN 0xffff000000000000L
#define KERNEL_SPACE_END 0xffffffffffffffffL

#define USER_SPACE_BEGIN 0x0000000000400000L
#define USER_SPACE_END 0x0000800000000000L

#define USER_STACK_TOP 0x00007fffffff0000L
#define USER_STACK_SIZE 0x800000 /* grows down up to that size limit */

#define VM_PHYSSEG_NMAX 16

#endif /* !_AARCH64_VM_PARAM_H_ */
