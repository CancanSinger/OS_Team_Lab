#ifndef __KERN_MM_SLUB_PMM_H__
#define __KERN_MM_SLUB_PMM_H__

#include <pmm.h>

extern const struct pmm_manager slub_pmm_manager;

// SLUB特定函数声明
void* slub_alloc_obj(size_t size);
void slub_free_obj(void *obj);

#endif /* ! __KERN_MM_SLUB_PMM_H__ */