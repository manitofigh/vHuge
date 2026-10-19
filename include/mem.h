// Memory allocation helpers
#ifndef MEM_H
#define MEM_H

#include "asm.h"
#include "common.h"

static ALWAYS_INLINE u8 *mmap_huge_private_init(void *addr, size_t size, u8 i)
{
    u8 *ptr = mmap(addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

    if (ptr == MAP_FAILED) {
        fprintf(stderr, ERR "HP allocation failed; is vm.nr_hugepages > 0?\n");
        return NULL;
    }
    memset(ptr, i, size);
    return ptr;
}

static ALWAYS_INLINE u8 *mmap_anon_thp_init(void *addr, size_t size, u8 init)
{
    u8 *ptr = (u8 *)mmap(addr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    if (madvise(ptr, size, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE)");
        munmap(ptr, size);
        return NULL;
    }

    memset(ptr, init, size);
    return ptr;
}

#endif // MEM_H
