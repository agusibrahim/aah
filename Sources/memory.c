#include "aah.h"

const char *mem_perm_str[] = {
    [0] = "none",
    [1] = "r--",
    [2] = "-w-",
    [3] = "rw-",
    [4] = "--x",
    [5] = "r-x",
    [6] = "-wx",
    [7] = "rwx"
};

const char *mem_get_tag(void *address) {
    Dl_info info;
    if (dladdr(address, &info)) {
        return info.dli_fname;
    } else {
        return "*";
    }
}

void mem_print_uc_regions(uc_engine *uc) {
    if (uc == NULL) {
        uc = get_emulator_ctx()->uc;
    }
    uc_mem_region *regions;
    uint32_t num_regions;
    
    uc_mem_regions(uc, &regions, &num_regions);
    qsort_b(regions, num_regions, sizeof(uc_mem_region), ^int(const void * rpa, const void * rpb) {
        const uc_mem_region *ra = (const uc_mem_region*)rpa;
        const uc_mem_region *rb = (const uc_mem_region*)rpb;
        return (int64_t)ra->begin - (int64_t)rb->end;
    });
    printf("%d regions:\n", num_regions);
    for(uint32_t i = 0; i < num_regions; i++) {
        printf("  %p->%p %s %s\n", (void*)regions[i].begin, (void*)regions[i].end, mem_perm_str[regions[i].perms], mem_get_tag((void*)regions[i].begin));
    }
    free(regions);
}

bool mem_remap_region(uc_engine *uc, uint64_t address, size_t size, uint32_t perms, uc_err *err_ptr) {
    uc_mem_region *regions;
    uint32_t num_regions;
    uint64_t end = address + size - 1;
    uc_mem_regions(uc, &regions, &num_regions);
    // find overlapping region and unmap it
    // assume regions only expand from the end
    for(uint32_t i = 0; i < num_regions; i++) {
        if ((address >= regions[i].begin && address <= regions[i].end) ||
            (end >= regions[i].begin && end <= regions[i].end)) {
            size_t region_size = regions[i].end - regions[i].begin + 1;
            perms |= regions[i].perms;
            uc_err err = uc_mem_unmap(uc, regions[i].begin, region_size);
            if (err != UC_ERR_OK) {
                printf("uc_mem_unmap(%p, 0x%lx): %s\n", (void*)regions[i].begin, region_size, uc_strerror(err));
                break;
            }
            err = uc_mem_map_ptr(uc, address, size, perms, (void*)address);
            if (err != UC_ERR_OK) {
                printf("uc_mem_map_ptr(%p, 0x%lx): %s\n", (void*)regions[i].begin, region_size, uc_strerror(err));
                break;
            }
            *err_ptr = err;
            free(regions);
            return true;
        }
    }
    fprintf(stderr, "mem_remap_region: did not find region\n");
    free(regions);
    return false;
}

bool mem_map_region_containing(uc_engine *uc, uint64_t address, uint32_t perms) {
    vm_address_t region_address = (vm_address_t)address;
    vm_size_t region_size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    memory_object_name_t object;
    kern_return_t err = vm_region_64(mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (err != KERN_SUCCESS) {
        return false;
    }
    
    if (address < region_address || address >= (region_address + region_size)) {
        fprintf(stderr, "could not map memory: no region found for %p\n", (void*)address);
        abort();
    }
    
    uc_err uerr = uc_mem_map_ptr(uc, region_address, region_size, perms, (void*)region_address);
    if (uerr != UC_ERR_OK) {
        printf("uc_mem_map_ptr(%p, 0x%lx, %s): %s, trying to remap\n", (void*)region_address, region_size, mem_perm_str[perms], uc_strerror(uerr));
        bool found = mem_remap_region(uc, region_address, region_size, perms, &uerr);
        if (uerr != UC_ERR_OK || !found) {
            printf("uc_mem_map_ptr(%p, 0x%lx, %s): %s\n", (void*)region_address, region_size, mem_perm_str[perms], uc_strerror(uerr));
            mem_print_uc_regions(uc);
            return false;
        }
    }
    
    return true;
}

hidden bool mem_is_mapped(uc_engine *uc, uint64_t address, size_t size, uint32_t perms) {
    uc_mem_region *regions;
    uint32_t num_regions;
    uint64_t end = address + size - 1;
    bool is_mapped = false;
    uc_mem_regions(uc, &regions, &num_regions);
    for(uint32_t i = 0; i < num_regions; i++) {
        if (address >= regions[i].begin && address <= regions[i].end && end <= regions[i].end) {
            is_mapped = (regions[i].perms & perms) ? true : false;
            break;
        }
    }
    free(regions);
    return is_mapped;
}

hidden void print_mem_info(void *ptr) {
    vm_address_t address = (vm_address_t)ptr;
    vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    memory_object_name_t object;
    kern_return_t err = vm_region_64(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    printf("region info for %p:\n", ptr);
    if (err != KERN_SUCCESS) {
        printf("  error: kern_return %d\n", err);
        return;
    }
    printf("  address = %p -> %p\n", (void*)address, (void*)(address+size));
    printf("  size = 0x%lx\n", size);
    printf("  offset = 0x%llx\n", info.offset);
    printf("  protection = (%x to %x)\n", info.protection, info.max_protection);
    
    Dl_info dl_info;
    if (dladdr(ptr, &dl_info)) {
        printf("  dli_fname = %s\n", dl_info.dli_fname);
        printf("  dli_fbase = %p\n", dl_info.dli_fbase);
        printf("  dli_sname = %s\n", dl_info.dli_sname);
        printf("  dli_saddr = %p\n", dl_info.dli_saddr);
    } else {
        printf("  no Dl_info");
    }
}
