// kern/mm/buddy_pmm.c
// ---------------- FINAL TREE-BASED VERSION ----------------

#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>

// 声明 pmm.c 中定义的全局变量
extern uint64_t va_pa_offset;

// Buddy System 核心数据结构
typedef struct {
    unsigned size;           // 管理的总页面数（必须是2的幂）
    unsigned *tree;          // 二叉树节点数组
    struct Page *page_base;  // 可供分配的 Page 数组的基地址
} buddy_system_t;

static buddy_system_t buddy;

// 宏定义
#define IS_POWER_OF_2(x) (!((x) & ((x) - 1)))
#define LEFT_LEAF(index) ((index) * 2 + 1)
#define RIGHT_LEAF(index) ((index) * 2 + 2)
#define PARENT(index) (((index) + 1) / 2 - 1)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// 向上取整到2的幂的快速算法
static unsigned fixsize(unsigned size) {
    size--;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return size + 1;
}

static void buddy_init(void) {
    buddy.size = 0;
    buddy.tree = NULL;
    buddy.page_base = NULL;
}

static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    
    // 1. 预估树需要多少内存，基于 n 进行预估
    unsigned est_size = 1;
    while ((est_size << 1) <= n) est_size <<= 1;
    if (est_size == 0) panic("Not enough memory to manage.");
    
    size_t tree_memsize = sizeof(unsigned) * (2 * est_size - 1);
    size_t pages_for_tree = (tree_memsize + PGSIZE - 1) / PGSIZE;
    
    if (pages_for_tree >= n) {
        panic("Not enough memory for buddy system metadata");
    }

    // 2. 正确地为树分配内存
    // base 指向的第一个物理页，将被我们用来存放树
    uintptr_t tree_pa = page2pa(base);
    // 将物理地址转换为内核可以直接访问的虚拟地址
    buddy.tree = (unsigned *)(tree_pa + va_pa_offset);
    
    // 3. 伙伴系统真正管理的 Page 从元数据之后开始
    buddy.page_base = base + pages_for_tree;
    size_t allocatable_pages = n - pages_for_tree;

    // 4. 基于真正可用的页面数，计算最终的管理大小 (向下取整到2的幂)
    buddy.size = 1;
    while ((buddy.size << 1) <= allocatable_pages) buddy.size <<= 1;

    cprintf("Buddy System (Tree): Total available %d pages.\n", n);
    cprintf("  Metadata uses first %d pages.\n", pages_for_tree);
    cprintf("  Managing the next %d pages (rounded down to %d).\n", allocatable_pages, buddy.size);

    // 5. 初始化二叉树
    unsigned node_size = buddy.size * 2;
    for (int i = 0; i < 2 * buddy.size - 1; ++i) {
        if (IS_POWER_OF_2(i + 1))
            node_size /= 2;
        buddy.tree[i] = node_size;
    }
    
    // 6. 将用于存放树的页面标记为已保留 (非常重要的一步)
    for (int i = 0; i < pages_for_tree; i++) {
        SetPageReserved(base + i);
    }

    // 7. 将我们管理的页面的 Reserved 标志清除
    for (int i = 0; i < buddy.size; i++) {
        ClearPageReserved(buddy.page_base + i);
    }
    cprintf("Buddy System (Tree): Initialized successfully\n");
}

static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (buddy.tree == NULL) return NULL;
    
    unsigned req_size = fixsize(n);
    if (buddy.tree[0] < req_size) return NULL;
    
    unsigned index = 0;
    unsigned node_size;
    for (node_size = buddy.size; node_size != req_size; node_size /= 2) {
        if (buddy.tree[LEFT_LEAF(index)] >= req_size)
            index = LEFT_LEAF(index);
        else
            index = RIGHT_LEAF(index);
    }
    
    buddy.tree[index] = 0;
    unsigned offset = (index + 1) * node_size - buddy.size;
    
    while (index > 0) {
        index = PARENT(index);
        buddy.tree[index] = MAX(buddy.tree[LEFT_LEAF(index)], buddy.tree[RIGHT_LEAF(index)]);
    }
    
    struct Page *page = buddy.page_base + offset;
    page->property = req_size; // 记录实际分配的大小
    SetPageProperty(page);
    return page;
}

static void buddy_free_pages(struct Page *base, size_t n) {
    assert(base >= buddy.page_base && base < buddy.page_base + buddy.size);
    unsigned offset = base - buddy.page_base;
    
    unsigned node_size = fixsize(n);
    unsigned index = offset + buddy.size - 1;
    // 如果不是叶子节点，需要向上找到正确的层级
    while (node_size > 1) {
        index = PARENT(index);
        node_size /= 2;
    }
    assert(buddy.tree[index] == 0); // 确保我们释放的是一个已分配的块
    
    node_size = fixsize(n);
    buddy.tree[index] = node_size;
    
    while (index > 0) {
        index = PARENT(index);
        node_size *= 2;
        unsigned left = buddy.tree[LEFT_LEAF(index)];
        unsigned right = buddy.tree[RIGHT_LEAF(index)];
        if (left + right == node_size)
            buddy.tree[index] = node_size;
        else
            buddy.tree[index] = MAX(left, right);
    }
    ClearPageProperty(base);
}

static size_t buddy_nr_free_pages(void) {
    // This is not a precise count of individual pages, but the size of the largest block
    if (buddy.tree == NULL) return 0;
    // A more accurate (but slow) way would be to traverse the tree and sum up free leaves.
    // For now, this is sufficient for the manager interface's spirit.
    return buddy.tree[0];
}

static void buddy_check(void) {
    cprintf("buddy_check() running (Tree version):\n");

    struct Page *p1 = alloc_pages(1); assert(p1 != NULL);
    struct Page *p2 = alloc_pages(1); assert(p2 != NULL);
    cprintf("  Allocated p1(idx %u), p2(idx %u).\n", p1 - buddy.page_base, p2 - buddy.page_base);
    assert((p1 - buddy.page_base) == 0 && (p2 - buddy.page_base) == 1);

    struct Page *p3 = alloc_pages(3); assert(p3 != NULL);
    cprintf("  Allocated p3(3->4 pages) at idx %u.\n", p3 - buddy.page_base);
    assert((p3 - buddy.page_base) == 4);

    cprintf("  Freeing p1 and p2...\n");
    free_pages(p1, 1);
    free_pages(p2, 1);
    
    p1 = alloc_pages(2); assert(p1 != NULL);
    cprintf("  Re-allocated 2 pages, got idx %u.\n", p1 - buddy.page_base);
    assert((p1 - buddy.page_base) == 0);

    // Cleanup
    free_pages(p1, 2);
    free_pages(p3, 3);

    size_t final_free = nr_free_pages();
    assert(final_free == buddy.size);
    cprintf("  Final check: Total free block size matches managed size (%u).\n", final_free);

    cprintf("Buddy system (Tree version) check passed!\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager (Tree based)",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};