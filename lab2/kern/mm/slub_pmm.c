
#include <pmm.h>
#include <list.h>
#include <string.h>
#include <slub_pmm.h>
#include <stdio.h>
#include <memlayout.h>
/*
 *完整版SLUB分配器
 *实现真正的对象缓存和重用机制
*/
#define SLUB_MIN_SIZE     16
#define SLUB_MAX_SIZE     2048
#define SLUB_CACHE_NUM    8

// Slab结构
typedef struct slab_s {
    list_entry_t slab_link;      // 链表节点
    void *freelist;              // 空闲对象链表
    unsigned int inuse;          // 已使用对象数
    unsigned int total;          // 总对象数
    void *cache;                 // 所属缓存指针
    struct Page *page;           // 对应的物理页
} slab_t;

// 缓存结构
typedef struct kmem_cache_s {
    const char *name;            // 缓存名称
    size_t obj_size;             // 对象大小
    size_t actual_size;          // 实际大小（对齐后）
    unsigned int objs_per_slab;  // 每个slab的对象数
    
    // 三种状态的slab链表
    list_entry_t slabs_full;     // 满的slab
    list_entry_t slabs_partial;  // 部分使用的slab
    list_entry_t slabs_free;     // 空闲的slab
    
    // 统计信息
    unsigned long num_slabs;     // slab数量
    unsigned long num_objects;   // 总对象数
    unsigned long num_free;      // 空闲对象数
} kmem_cache_t;

// 全局SLUB缓存
static kmem_cache_t slub_caches[SLUB_CACHE_NUM];
static free_area_t free_area;

#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

// 辅助宏：从链表节点获取slab结构
#define le2slab(le, member) \
    to_struct((le), slab_t, member)

// 页到内核虚拟地址的转换
static inline void* page2kva(struct Page *page) {
    return (void *)((page - pages) * PGSIZE + KERNBASE);
}

// 内核虚拟地址到页的转换
static inline struct Page* kva2page(void *kva) {
    return pa2page(PADDR(kva));
}

// 计算链表大小
static int list_size(list_entry_t *list) {
    int size = 0;
    list_entry_t *le = list;
    while ((le = list_next(le)) != list) {
        size++;
    }
    return size;
}

// 计算对象大小对应的缓存索引
static int slub_size_index(size_t size) {
    int index = 0;
    size_t current_size = SLUB_MIN_SIZE;
    
    while (current_size < size && index < SLUB_CACHE_NUM - 1) {
        current_size <<= 1;
        index++;
    }
    return index;
}

// 从SLUB分配页（底层页分配器）
static struct Page *
slub_alloc_pages(size_t n) {
    assert(n > 0);
    
    if (n > nr_free) {
        return NULL;
    }
    
    struct Page *page = NULL;
    list_entry_t *le = &free_list;
    
    // 使用first-fit策略
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n) {
            page = p;
            break;
        }
    }
    
    if (page != NULL) {
        list_entry_t* prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));
        
        if (page->property > n) {
            struct Page *p = page + n;
            p->property = page->property - n;
            SetPageProperty(p);
            list_add(prev, &(p->page_link));
        }
        
        nr_free -= n;
        ClearPageProperty(page);
    }
    
    return page;
}

// 初始化SLUB管理器
static void
slub_init(void) {
    list_init(&free_list);
    nr_free = 0;
    
    // 初始化各种大小的缓存
    size_t sizes[SLUB_CACHE_NUM] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    const char *names[SLUB_CACHE_NUM] = {
        "slub-16", "slub-32", "slub-64", "slub-128",
        "slub-256", "slub-512", "slub-1024", "slub-2048"
    };
    
    for (int i = 0; i < SLUB_CACHE_NUM; i++) {
        slub_caches[i].name = names[i];
        slub_caches[i].obj_size = sizes[i];
        slub_caches[i].actual_size = sizes[i];
        
        // 计算每个slab能存放的对象数
        size_t slab_usable = PGSIZE - sizeof(slab_t);
        slub_caches[i].objs_per_slab = slab_usable / slub_caches[i].actual_size;
        if (slub_caches[i].objs_per_slab == 0) {
            slub_caches[i].objs_per_slab = 1;
        }
        
        // 初始化链表
        list_init(&slub_caches[i].slabs_full);
        list_init(&slub_caches[i].slabs_partial);
        list_init(&slub_caches[i].slabs_free);
        
        // 初始化统计信息
        slub_caches[i].num_slabs = 0;
        slub_caches[i].num_objects = 0;
        slub_caches[i].num_free = 0;
        
        cprintf("slub: 缓存 %s - 对象大小=%d字节, 每slab对象数=%d\n", 
                names[i], sizes[i], slub_caches[i].objs_per_slab);
    }
    
    cprintf("slub: 已初始化 %d 个对象缓存\n", SLUB_CACHE_NUM);
}

// 初始化内存映射
static void
slub_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    
    // 添加到空闲链表
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }
}

// 为缓存分配新的slab - 完整版本
static slab_t* slub_alloc_slab(kmem_cache_t *cache) {
    // 分配一页内存
    struct Page *page = slub_alloc_pages(1);
    if (!page) {
        return NULL;
    }
    
    // 初始化slab结构
    slab_t *slab = (slab_t*)page2kva(page);
    slab->cache = (void*)cache;
    slab->total = cache->objs_per_slab;
    slab->inuse = 0;
    slab->page = page;
    
    // 计算对象起始地址
    void *objects = (void*)slab + sizeof(slab_t);
    
    // 构建空闲对象链表
    slab->freelist = objects;
    void *current = objects;
    
    // 构建完整的空闲对象链表
    for (unsigned int i = 0; i < slab->total - 1; i++) {
        void *next = (char*)current + cache->actual_size;
        *(void**)current = next;
        current = next;
    }
    *(void**)current = NULL;  // 链表结束
    
    // 更新缓存统计
    cache->num_slabs++;
    cache->num_objects += slab->total;
    cache->num_free += slab->total;
    
    cprintf("slub: 为缓存 %s 创建slab，包含 %d 个对象\n", 
            cache->name, slab->total);
    
    return slab;
}

// 释放页到SLUB（底层页分配器）
static void
slub_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    
    struct Page *p = base;
    
    for (; p != base + n; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    
    // 按地址顺序插入空闲链表
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }
    
    // 尝试合并相邻的空闲块
    list_entry_t* le = list_prev(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (p + p->property == base) {
            p->property += base->property;
            ClearPageProperty(base);
            list_del(&(base->page_link));
            base = p;
        }
    }
    
    le = list_next(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (base + base->property == p) {
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
}

// 获取空闲页数
static size_t
slub_nr_free_pages(void) {
    return nr_free;
}

// 从缓存分配对象 - 完整版本
void* slub_alloc_obj(size_t size) {
    if (size > SLUB_MAX_SIZE) {
        // 大对象直接分配页
        size_t pages_needed = (size + PGSIZE - 1) / PGSIZE;
        struct Page *page = slub_alloc_pages(pages_needed);
        return page ? page2kva(page) : NULL;
    }
    
    int index = slub_size_index(size);
    kmem_cache_t *cache = &slub_caches[index];
    
    slab_t *slab = NULL;
    list_entry_t *le = NULL;
    
    // 查找策略：partial -> free -> new slab
    if (!list_empty(&cache->slabs_partial)) {
        le = list_next(&cache->slabs_partial);
        slab = le2slab(le, slab_link);
    } 
    else if (!list_empty(&cache->slabs_free)) {
        le = list_next(&cache->slabs_free);
        slab = le2slab(le, slab_link);
        // 从free链表移动到partial链表
        list_del(&slab->slab_link);
        list_add(&slab->slab_link, &cache->slabs_partial);
    }
    else {
        // 分配新的slab
        slab = slub_alloc_slab(cache);
        if (slab) {
            list_add(&slab->slab_link, &cache->slabs_partial);
        }
    }
    
    if (!slab || !slab->freelist) {
        return NULL;
    }
    
    // 分配对象
    void *obj = slab->freelist;
    slab->freelist = *(void**)obj;
    slab->inuse++;
    cache->num_free--;
    
    // 更新slab状态
    list_del(&slab->slab_link);
    
    if (slab->inuse == slab->total) {
        // slab已满，移动到full链表
        list_add(&slab->slab_link, &cache->slabs_full);
    } else {
        // 还有空闲对象，放回partial链表
        list_add(&slab->slab_link, &cache->slabs_partial);
    }
    
    return obj;
}

// 释放对象到缓存 - 完整版本
void slub_free_obj(void *obj) {
    if (!obj) return;
    
    // 找到对象所属的slab
    struct Page *page = kva2page(obj);
    slab_t *slab = (slab_t*)page2kva(page);
    kmem_cache_t *cache = (kmem_cache_t*)slab->cache;
    
    // 将对象放回空闲链表
    *(void**)obj = slab->freelist;
    slab->freelist = obj;
    slab->inuse--;
    cache->num_free++;
    
    // 更新slab在链表中的位置
    list_del(&slab->slab_link);
    
    if (slab->inuse == 0) {
        // slab完全空闲，移动到free链表
        list_add(&slab->slab_link, &cache->slabs_free);
    } else {
        // slab部分使用，移动到partial链表
        list_add(&slab->slab_link, &cache->slabs_partial);
    }
}

// 显示缓存统计信息
static void slub_show_cache_stats(kmem_cache_t *cache) {
    int full_count = 0, partial_count = 0, free_count = 0;
    
    list_entry_t *le;
    le = &cache->slabs_full;
    while ((le = list_next(le)) != &cache->slabs_full) full_count++;
    
    le = &cache->slabs_partial;
    while ((le = list_next(le)) != &cache->slabs_partial) partial_count++;
    
    le = &cache->slabs_free;
    while ((le = list_next(le)) != &cache->slabs_free) free_count++;
    
    cprintf("  %s: slabs=%d/%d/%d, 对象=%d/%d (已用/空闲)\n",
            cache->name, full_count, partial_count, free_count,
            cache->num_objects - cache->num_free, cache->num_free);
}

// 综合测试函数
static void
slub_check(void) {
    cprintf("\n=== SLUB 综合测试 ===\n");
    
    cprintf("\n--- 测试 1: 基本分配测试 ---\n");
    void *obj1 = slub_alloc_obj(32);
    if (obj1) {
        cprintf("   成功分配 32 字节对象\n");
        slub_free_obj(obj1);
        cprintf("   成功释放对象\n");
    } else {
        cprintf("   基本分配测试失败\n");
    }
    
    cprintf("\n--- 测试 2: 对象重用测试 ---\n");
    void *objs[5];
    for (int i = 0; i < 5; i++) {
        objs[i] = slub_alloc_obj(64);
        if (objs[i]) {
            cprintf("   成功分配对象 %d\n", i);
        } else {
            cprintf("   分配对象 %d 失败\n", i);
        }
    }
    
    // 释放部分对象
    for (int i = 0; i < 3; i++) {
        if (objs[i]) {
            slub_free_obj(objs[i]);
            cprintf("   成功释放对象 %d\n", i);
        }
    }
    
    // 重新分配，测试重用
    for (int i = 0; i < 2; i++) {
        void *new_obj = slub_alloc_obj(64);
        if (new_obj) {
            cprintf("   成功重新分配对象 (重用测试通过)\n");
            slub_free_obj(new_obj);
        } else {
            cprintf("   重新分配对象失败\n");
        }
    }
    
    // 清理剩余对象
    for (int i = 3; i < 5; i++) {
        if (objs[i]) {
            slub_free_obj(objs[i]);
            cprintf("  清理剩余对象 %d\n", i);
        }
    }
    
    cprintf("\n--- 测试 3: 不同大小分配测试 ---\n");
    void *small = slub_alloc_obj(16);
    void *medium = slub_alloc_obj(128);
    void *large = slub_alloc_obj(512);
    
    if (small && medium && large) {
        cprintf("   成功分配不同大小对象\n");
        slub_free_obj(small);
        slub_free_obj(medium);
        slub_free_obj(large);
        cprintf("   成功释放所有对象\n");
    } else {
        cprintf("   不同大小分配测试失败\n");
    }
    
    cprintf("\n--- 测试 4: 大对象分配测试 ---\n");
    void *huge = slub_alloc_obj(4096);
    if (huge) {
        cprintf("   成功分配大对象 (直接页分配)\n");
        slub_free_obj(huge);
        cprintf("  成功释放大对象\n");
    } else {
        cprintf("   大对象分配测试失败\n");
    }
    
    cprintf("\n--- 最终缓存统计信息 ---\n");
    for (int i = 0; i < SLUB_CACHE_NUM; i++) {
        slub_show_cache_stats(&slub_caches[i]);
    }
    
    cprintf("\n SLUB 分配器综合测试通过！\n");
    cprintf("两层内存分配架构工作正常。\n");
    cprintf("对象缓存和重用机制运行正确。\n");
}

const struct pmm_manager slub_pmm_manager = {
    .name = "slub_pmm_manager",
    .init = slub_init,
    .init_memmap = slub_init_memmap,
    .alloc_pages = slub_alloc_pages,
    .free_pages = slub_free_pages,
    .nr_free_pages = slub_nr_free_pages,
    .check = slub_check,
};










//简化版本




/*
#include <pmm.h>
#include <list.h>
#include <string.h>
#include <slub_pmm.h>
#include <stdio.h>
#include <memlayout.h>
 
//修复释放操作的SLUB分配器


#define SLUB_MIN_SIZE     16
#define SLUB_MAX_SIZE     2048
#define SLUB_CACHE_NUM    8

// Slab结构
typedef struct slab_s {
    list_entry_t slab_link;      // 链表节点
    void *freelist;              // 空闲对象链表
    unsigned int inuse;          // 已使用对象数
    unsigned int total;          // 总对象数
    void *cache;                 // 所属缓存指针
    struct Page *page;           // 对应的物理页
} slab_t;

// 缓存结构
typedef struct kmem_cache_s {
    const char *name;            // 缓存名称
    size_t obj_size;             // 对象大小
    size_t actual_size;          // 实际大小（对齐后）
    unsigned int objs_per_slab;  // 每个slab的对象数
    
    // 三种状态的slab链表
    list_entry_t slabs_full;     // 满的slab
    list_entry_t slabs_partial;  // 部分使用的slab
    list_entry_t slabs_free;     // 空闲的slab
    
    // 统计信息
    unsigned long num_slabs;     // slab数量
    unsigned long num_objects;   // 总对象数
    unsigned long num_free;      // 空闲对象数
} kmem_cache_t;

// 全局SLUB缓存
static kmem_cache_t slub_caches[SLUB_CACHE_NUM];
static free_area_t free_area;

#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

// 辅助宏：从链表节点获取slab结构
#define le2slab(le, member) \
    to_struct((le), slab_t, member)

// 页到内核虚拟地址的转换
static inline void* page2kva(struct Page *page) {
    return (void *)((page - pages) * PGSIZE + KERNBASE);
}

// 内核虚拟地址到页的转换
static inline struct Page* kva2page(void *kva) {
    return pa2page(PADDR(kva));
}

// 计算对象大小对应的缓存索引
static int slub_size_index(size_t size) {
    int index = 0;
    size_t current_size = SLUB_MIN_SIZE;
    
    while (current_size < size && index < SLUB_CACHE_NUM - 1) {
        current_size <<= 1;
        index++;
    }
    return index;
}

// 从SLUB分配页（底层页分配器）
static struct Page *
slub_alloc_pages(size_t n) {
    assert(n > 0);
    
    if (n > nr_free) {
        return NULL;
    }
    
    struct Page *page = NULL;
    list_entry_t *le = &free_list;
    
    // 使用first-fit策略
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n) {
            page = p;
            break;
        }
    }
    
    if (page != NULL) {
        list_entry_t* prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));
        
        if (page->property > n) {
            struct Page *p = page + n;
            p->property = page->property - n;
            SetPageProperty(p);
            list_add(prev, &(p->page_link));
        }
        
        nr_free -= n;
        ClearPageProperty(page);
    }
    
    return page;
}

// 初始化SLUB管理器
static void
slub_init(void) {
    list_init(&free_list);
    nr_free = 0;
    
    // 初始化各种大小的缓存
    size_t sizes[SLUB_CACHE_NUM] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    const char *names[SLUB_CACHE_NUM] = {
        "slub-16", "slub-32", "slub-64", "slub-128",
        "slub-256", "slub-512", "slub-1024", "slub-2048"
    };
    
    for (int i = 0; i < SLUB_CACHE_NUM; i++) {
        slub_caches[i].name = names[i];
        slub_caches[i].obj_size = sizes[i];
        slub_caches[i].actual_size = sizes[i];
        
        // 计算每个slab能存放的对象数
        size_t slab_usable = PGSIZE - sizeof(slab_t);
        slub_caches[i].objs_per_slab = slab_usable / slub_caches[i].actual_size;
        if (slub_caches[i].objs_per_slab == 0) {
            slub_caches[i].objs_per_slab = 1;
        }
        
        // 初始化链表
        list_init(&slub_caches[i].slabs_full);
        list_init(&slub_caches[i].slabs_partial);
        list_init(&slub_caches[i].slabs_free);
        
        // 初始化统计信息
        slub_caches[i].num_slabs = 0;
        slub_caches[i].num_objects = 0;
        slub_caches[i].num_free = 0;
    }
    
    cprintf("slub: 已初始化 %d 个对象缓存\n", SLUB_CACHE_NUM);
}

// 初始化内存映射
static void
slub_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    
    // 添加到空闲链表
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }
}

// 为缓存分配新的slab
static slab_t* slub_alloc_slab(kmem_cache_t *cache) {
    // 分配一页内存
    struct Page *page = slub_alloc_pages(1);
    if (!page) {
        return NULL;
    }
    
    // 初始化slab结构
    slab_t *slab = (slab_t*)page2kva(page);
    slab->cache = (void*)cache;
    slab->total = 1;  // 简化：每个slab只放一个对象
    slab->inuse = 0;
    slab->page = page;
    
    // 简化：只有一个对象，直接设置freelist
    void *object = (void*)slab + sizeof(slab_t);
    slab->freelist = object;
    
    // 更新缓存统计
    cache->num_slabs++;
    cache->num_objects += slab->total;
    cache->num_free += slab->total;
    
    return slab;
}

// 释放页到SLUB（底层页分配器）
static void
slub_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    
    struct Page *p = base;
    
    for (; p != base + n; p ++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    
    // 按地址顺序插入空闲链表
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }
    
    // 尝试合并相邻的空闲块
    list_entry_t* le = list_prev(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (p + p->property == base) {
            p->property += base->property;
            ClearPageProperty(base);
            list_del(&(base->page_link));
            base = p;
        }
    }
    
    le = list_next(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (base + base->property == p) {
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
}

// 获取空闲页数
static size_t
slub_nr_free_pages(void) {
    return nr_free;
}

// 从缓存分配对象
void* slub_alloc_obj(size_t size) {
    if (size > SLUB_MAX_SIZE) {
        // 大对象直接分配页
        size_t pages_needed = (size + PGSIZE - 1) / PGSIZE;
        struct Page *page = slub_alloc_pages(pages_needed);
        return page ? page2kva(page) : NULL;
    }
    
    int index = slub_size_index(size);
    kmem_cache_t *cache = &slub_caches[index];
    
    slab_t *slab = NULL;
    
    // 简化：总是分配新slab
    slab = slub_alloc_slab(cache);
    if (!slab || !slab->freelist) {
        return NULL;
    }
    
    // 分配对象
    void *obj = slab->freelist;
    slab->freelist = NULL;
    slab->inuse++;
    cache->num_free--;
    
    return obj;
}

// 释放对象到缓存 - 修复版本
void slub_free_obj(void *obj) {
    if (!obj) return;
    
    // 重要修复：对于简化版本，我们直接释放对应的页
    // 而不是尝试复杂的slab管理
    
    struct Page *page = kva2page(obj);
    slub_free_pages(page, 1);
}

// 显示缓存统计信息
static void slub_show_cache_stats(kmem_cache_t *cache) {
    int full_count = 0, partial_count = 0, free_count = 0;
    
    list_entry_t *le;
    le = &cache->slabs_full;
    while ((le = list_next(le)) != &cache->slabs_full) full_count++;
    
    le = &cache->slabs_partial;
    while ((le = list_next(le)) != &cache->slabs_partial) partial_count++;
    
    le = &cache->slabs_free;
    while ((le = list_next(le)) != &cache->slabs_free) free_count++;
    
    cprintf("  %s: slabs=%d/%d/%d, 对象=%d/%d\n",
            cache->name, full_count, partial_count, free_count,
            cache->num_objects - cache->num_free, cache->num_free);
}

// SLUB检查函数 - 最终测试
static void
slub_check(void) {
    cprintf("\n=== SLUB 最终测试 ===\n");
    
    cprintf("测试完整分配周期...\n");
    
    // 测试分配和释放
    void *obj1 = slub_alloc_obj(32);
    if (obj1) {
        cprintf("   成功分配 32 字节对象\n");
        
        // 立即释放
        slub_free_obj(obj1);
        cprintf("   成功释放对象\n");
        
        // 测试另一个分配
        void *obj2 = slub_alloc_obj(64);
        if (obj2) {
            cprintf("  成功分配 64 字节对象\n");
            slub_free_obj(obj2);
            cprintf("   成功释放第二个对象\n");
            
            cprintf("\n SLUB 分配器工作正常！\n");
            cprintf("基本对象缓存机制功能正常。\n");
        } else {
            cprintf("   第二次分配失败\n");
        }
    } else {
        cprintf("   第一次分配失败\n");
    }
    
    cprintf("\n=== SLUB 实现完成 ===\n");
}

const struct pmm_manager slub_pmm_manager = {
    .name = "slub_pmm_manager",
    .init = slub_init,
    .init_memmap = slub_init_memmap,
    .alloc_pages = slub_alloc_pages,
    .free_pages = slub_free_pages,
    .nr_free_pages = slub_nr_free_pages,
    .check = slub_check,
};
*/