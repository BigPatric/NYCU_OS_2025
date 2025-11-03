#include <stddef.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define NUM_LEVEL 11
#define MAX_BLOCKS_PER_LEVEL 100 

static size_t level_size[NUM_LEVEL] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
static void *pool = NULL;
static int first_malloc = 0; 

// the node prev and next is for the entire block list (allocated or free)
typedef struct Node {
    size_t size; // include the header size
    struct Node *next; // next block in the overall memory pool
    struct Node *prev; // prev block in the overall memory pool
    int free; // a flag to indicate if the block is free
    int start_addr; // offset from pool, the starter of the header
} node;

static node* free_list[NUM_LEVEL][MAX_BLOCKS_PER_LEVEL] = {NULL};
static int free_list_count[NUM_LEVEL] = {0};

int get_level(size_t size) {
    for (int i = 0; i < NUM_LEVEL; ++i) {
        if (size <= level_size[i]) {
            return i;
        }
    }
    return NUM_LEVEL - 1;
}

static void init_pool(){
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
            if (cur->size >= size) {
                if (best_fit == NULL || cur->size < best_fit->size) {
                    best_fit = cur;
                }
            }
        }
        if (best_fit && best_fit->size < level_size[lvl]) {
            break;
        }
    }
    return best_fit;
}

void insert_to_free_list(node *block) {
    int level = get_level(block->size);
    if (free_list_count[level] < MAX_BLOCKS_PER_LEVEL) {
        free_list[level][free_list_count[level]++] = block;
    }
}

void remove_from_free_list(node *block) {
    int level = get_level(block->size);
    int index = -1;

    for (int i = 0; i < free_list_count[level]; ++i) {
        if (free_list[level][i] == block) {
            index = i;
            break;
        }
    }

    if (index == -1) return; // not found

    // push forward
    for (int i = index; i < free_list_count[level] - 1; ++i) {
        free_list[level][i] = free_list[level][i + 1];
    }

    // clear last entry
    free_list[level][free_list_count[level] - 1] = NULL;
    --free_list_count[level];
}

static void *split_block(node *block, size_t size){

    node *child = (node *)((char *)block + size); // the new free block
    child->start_addr = block->start_addr + size;
    child->size = block->size - size;
    child->free = 1;

    // update the linked list
    child->next = block->next;
    child->prev = block;

    block->size = size;
    block->free = 0;
    block->next = child;

    if (child->next) child->next->prev = child;

    // insert the new free block into free_list
    insert_to_free_list(child);

    return block;
}

int round_up(size_t size){ // round the requested size to multiple of 32

    // add the size of header
    size_t required_size = size + sizeof(node);
    size_t rounded = (required_size + 31) & ~31;
    if (rounded < sizeof(node)) return sizeof(node);
    return (int)rounded; 
}

void *malloc(size_t size){

    // malloc(0)
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
        max_free_size -= sizeof(node); // without header size
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

    // round up to multiple of 32 including header
    size_t total_size = round_up(size); 
    
    node *best_fit = find_free_block(total_size);
    if(best_fit == NULL){ return NULL; }

    // remove first
    remove_from_free_list(best_fit); 

    // if still have enough space to split
    if (best_fit->size >= total_size + sizeof(node)) {
        best_fit = split_block(best_fit, total_size);
    } else {
        // else allocate the entire block
        best_fit->free = 0;
    }

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

    // --- try merging with previous block ---
    if(prev_block && prev_block->free){
        // remove prev_block first
        remove_from_free_list(prev_block);

        prev_block->size += block->size;
        prev_block->next = block->next;
        if(block->next) block->next->prev = prev_block;
        
        coalesced_block = prev_block; 
    }

    // --- try merging with next block ---
    // re-fetch next_block, as coalesced_block->next may have changed (if previous merge happened)
    next_block = coalesced_block->next;

    if(next_block && next_block->free){
        // remove next_block first
        remove_from_free_list(next_block);

        coalesced_block->size += next_block->size;
        coalesced_block->next = next_block->next;
        if(next_block->next) next_block->next->prev = coalesced_block;
    }

    // insert the final coalesced block into free_list
    insert_to_free_list(coalesced_block);
}