#ifndef _SYS_FDT_H_
#define _SYS_FDT_H_

#include <sys/types.h>

#define FDT_MAX_ADDR_CELLS 2
#define FDT_MAX_SIZE_CELLS 2
#define FDT_REG_CELLS (FDT_MAX_ADDR_CELLS + FDT_MAX_SIZE_CELLS)

typedef uint32_t phandle_t;
typedef uint32_t pcall_t;

#define FDT_NODEV ((phandle_t)-1)

typedef fdt_mem_reg {
  paddr_t addr;
  size_t size;
} fdt_mem_reg_t;

#endif /* !_SYS_FDT_H_ */
