#include "../include/gpa_hpa.h"
#include "../include/utils.h"
#include <errno.h>

static i32 debug_mod_fd = -1;

// guest physical address of a mapped virtual address, from /proc/self/pagemap
static u64 va_to_pa(void* va)
{
    FILE *pagemap = fopen("/proc/self/pagemap", "rb");
    if (!pagemap) {
        perror("fopen");
        return 0;
    }
    u64 page_size = getpagesize();
    u64 offset = ((u64)va / page_size) * sizeof(u64);
    if (fseek(pagemap, offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(pagemap);
        return 0;
    }
    u64 entry = 0;
    if (fread(&entry, sizeof(u64), 1, pagemap) != 1) {
        perror("fread");
        fclose(pagemap);
        return 0;
    }
    fclose(pagemap);
    if (!(entry & (1ULL << 63))) {
        return 0;
    }
    u64 pfn = entry & ((1ULL << 55) - 1);
    u64 pa = (pfn << 12) | ((u64)va & (page_size - 1));
    return pa;
}

i32 start_debug_mod(void)
{
    debug_mod_fd = open(DEBUG_MOD_PATH, O_RDWR);
    if (debug_mod_fd < 0) {
        fprintf(stderr, ERR "Cannot open " DEBUG_MOD_PATH ". Is gpa_hpa loaded?\n");
        return -1;
    }
    if (verbose > 1)
        printf(V2 "set up debug module successfully\n");
    return 0;
}

void stop_debug_mod(void)
{
    close(debug_mod_fd);
    if (verbose > 1)
        printf(V2 "closed debug module's file descriptor\n");
}

i32 va_to_hpa_info(void* va, HpaInfo* out)
{
    u64 offset = (unsigned long)va & (PAGE_SIZE - 1);
    char buf[256];
    u64 gpa = va_to_pa(va);
    ull hpa_raw = 0;
    ull pfn_raw = 0;
    ull flags_raw = 0;
    ssize_t ret;

    memset(out, 0, sizeof(*out));

    /* proc write */
    snprintf(buf, sizeof(buf), "%llx", (ull)gpa);
    if (write(debug_mod_fd, buf, strlen(buf)) < 0) {
        if (errno == EBADF)
            fprintf(stderr, ERR "Cannot open /proc/gpa_hpa. Is gpa_hpa loaded?\n");
        else
            perror("failed write to proc");
        return -1;
    }

    /* read result right after */
    memset(buf, 0, sizeof(buf));
    ret = read(debug_mod_fd, buf, sizeof(buf) - 1);
    if (ret < 0) {
        perror("failed read from proc");
        return -1;
    }
    buf[ret] = '\0';

    if (sscanf(buf, "HPA=0x%llx PFN=0x%llx FLAGS=0x%llx",
               &hpa_raw, &pfn_raw, &flags_raw) != 3) {
        fprintf(stderr, ERR "Failed to parse HPA response: %s\n", buf);
        return -1;
    }
    out->hpa = ((u64)hpa_raw) | offset;
    out->flags = (u64)flags_raw;
    return 0;
}
