#include <stddef.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define NUM_LEVEL 11
#define MAX_BLOCKS_PER_LEVEL 100 // 每層最多允許的空閒區塊數量

static size_t level_size[NUM_LEVEL] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
static void *pool = NULL;
static int first_malloc = 0; 

// the node prev and next is for the entire block list (allocated or free)
typedef struct Node {
    size_t size; // include the header size
    struct Node *next; // next block in the overall memory pool
    struct Node *prev; // prev block in the overall memory pool
    int free;
    int start_addr; // offset from pool, the starter of the header
} node;

// 二維陣列的 free_list
static node* free_list[NUM_LEVEL][MAX_BLOCKS_PER_LEVEL] = {NULL};
static int free_list_count[NUM_LEVEL] = {0}; // 每層當前的空閒區塊數量

int get_level(size_t size) {
    for (int i = 0; i < NUM_LEVEL; ++i) {
        if (size <= level_size[i]) {
            return i;
        }
    }
    return NUM_LEVEL - 1;
}

static void init_pool(){
    // 使用 64KB (0x10000) 更常見的頁面大小整數倍，但保持原來的 20000 避免超出預期
    pool = mmap(NULL, 20000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if(pool == MAP_FAILED){pool = NULL; return;}
    first_malloc = 1;
    node *initial_node = (node *)pool;
    initial_node->start_addr = 0;
    initial_node->size = 20000;
    initial_node->free = 1;
    initial_node->next = NULL;
    initial_node->prev = NULL;
    int level = get_level(initial_node->size); 

    free_list[level][free_list_count[level]++] = initial_node;
}

static void *find_free_block(size_t size){
    node *best_fit = NULL;
    int start_lvl = get_level(size);

    for(int lvl = start_lvl; lvl < NUM_LEVEL; ++lvl){
        for (int i = 0; i < free_list_count[lvl]; ++i) {
            node *cur = free_list[lvl][i];
            // 由於分級列表設計，這裡的最佳適配其實就是 First Fit in the Best Level
            // 為了保持 Best Fit 邏輯，仍掃描整個區間，但應優先從小級別開始
            if (cur->size >= size) {
                if (best_fit == NULL || cur->size < best_fit->size) {
                    best_fit = cur;
                }
            }
        }
        if (best_fit && best_fit->size < level_size[lvl]) {
            // 在當前級別找到了一個剛好的區塊 (Best-Fit within the level)
            break;
        }
    }
    return best_fit;
}

void insert_to_free_list(node *block) {
    int level = get_level(block->size);
    if (free_list_count[level] < MAX_BLOCKS_PER_LEVEL) {
        free_list[level][free_list_count[level]++] = block;
    } else {
        // 在實際的分配器中，這是一個嚴重錯誤，但我們在此僅作提示
        fprintf(stderr, "Free list level %d is full! (Max %d blocks)\n", level, MAX_BLOCKS_PER_LEVEL);
    }
}

void remove_from_free_list(node *block) {
    int level = get_level(block->size);
    for (int i = 0; i < free_list_count[level]; ++i) {
        if (free_list[level][i] == block) {
            // 將最後一個元素移到當前位置，並減少計數 (Array based list removal)
            free_list[level][i] = free_list[level][free_list_count[level] - 1];
            free_list[level][free_list_count[level] - 1] = NULL;
            --free_list_count[level];
            return;
        }
    }
    // 區塊不在列表中，這是一個潛在的邏輯錯誤，例如嘗試釋放一個已經合併的區塊
    // fprintf(stderr, "Warning: Block to be removed not found in free list level %d.\n", level);
}

static void *split_block(node *block, size_t size){
    // block 已經在 malloc 中從 free_list 移除

    node *child = (node *)((char *)block + size); // 這是新的空閒區塊
    child->start_addr = block->start_addr + size;
    child->size = block->size - size;
    child->free = 1;

    // 更新整體鏈表結構
    child->next = block->next;
    child->prev = block;

    block->size = size;
    block->free = 0;
    block->next = child;

    if (child->next) child->next->prev = child;

    // 將新的空閒區塊 child 插入 free_list
    insert_to_free_list(child);

    return block;
}

int round_up(size_t size){ // round the requested size to multiple of 32
    // 必須考慮 node header 的大小
    size_t required_size = size + sizeof(node);
    size_t rounded = (required_size + 31) & ~31;
    // 確保總大小至少是 sizeof(node)
    if (rounded < sizeof(node)) return sizeof(node);
    return (int)rounded; 
}

void *malloc(size_t size){
    // 處理 malloc(0) 的特殊情況 (清理資源並返回 NULL)
    if(size == 0){
        size_t max_free_size = 0;
        for(int i = NUM_LEVEL-1; i >=0; --i){
            for (int j = 0; j < free_list_count[i]; ++j) {
                node *cur = free_list[i][j];
                if(cur->free && cur->size > max_free_size){
                    max_free_size = cur->size;
                }
            }
            if(max_free_size > 0) break;
        }
        max_free_size -= sizeof(node); // 不包含 header 的大小
        char buf[128];
        int n = snprintf((buf), sizeof(buf), "Max Free Chunk Size = %zu\n", max_free_size);
        if(n > 0) write(STDOUT_FILENO, buf, (size_t)n);

        if(pool){
            munmap(pool, 20000);
            pool = NULL;
            first_malloc = 0;
            for(int i = 0; i < NUM_LEVEL; ++i) {
                free_list_count[i] = 0;
            }
        }

        return NULL;
    }
    if(!first_malloc) init_pool();
    
    // 計算包含 header 後，且對齊到 32 位元組的總大小
    size_t total_size = round_up(size); 
    
    node *best_fit = find_free_block(total_size);
    if(best_fit == NULL){ return NULL; }

    // 無論是否切割，都先將 best_fit 從 free_list 移除
    remove_from_free_list(best_fit); 

    // 最小可切割的剩餘大小應至少能容納一個 node header
    if (best_fit->size >= total_size + sizeof(node)) {
        // 進行切割
        best_fit = split_block(best_fit, total_size);
    } else {
        // 不切割，直接分配整個區塊
        best_fit->free = 0;
    }

    // 返回有效載荷的起始地址
    return (void *)((char *)best_fit + sizeof(node));
}

void free(void *ptr){
    if(ptr == NULL) return;
    
    node *block = (node *)((char *)ptr - sizeof(node));
    if (block->free) {
        fprintf(stderr, "Warning: Double free detected!\n");
        return;
    }

    block->free = 1;
    node *coalesced_block = block; 

    node *prev_block = block->prev;
    node *next_block = block->next;

    // --- 嘗試與前一個區塊合併 ---
    if(prev_block && prev_block->free){
        // 必須先移除 prev_block，因為它的 size 和 level 將會改變
        remove_from_free_list(prev_block);

        prev_block->size += block->size;
        prev_block->next = block->next;
        if(block->next) block->next->prev = prev_block;
        
        coalesced_block = prev_block; 
    }

    // --- 嘗試與後一個區塊合併 ---
    // 重新取得 next_block，因為 coalesced_block->next 可能已經改變 (如果前一個合併發生)
    next_block = coalesced_block->next; 

    if(next_block && next_block->free){
        // 必須先移除 next_block，因為它將被銷毀
        remove_from_free_list(next_block);

        coalesced_block->size += next_block->size;
        coalesced_block->next = next_block->next;
        if(next_block->next) next_block->next->prev = coalesced_block;
    }

    // 將最終合併後的區塊插入 free_list
    insert_to_free_list(coalesced_block);
}