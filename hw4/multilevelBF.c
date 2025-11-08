#include <stddef.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#define NUM_LEVEL 11

static size_t level_size[NUM_LEVEL] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
static void *pool = NULL;
static int first_malloc = 0;

// the node structure for memory blocks
typedef struct Node {
    size_t size; // include the header size
    struct Node *next; // next block in the overall memory pool
    struct Node *free_next; // next block in the free list
    int free; // a flag to indicate if the block is free
    int start_addr; // offset from pool, the starter of the header
} node;

// 每層 free list 改為 linked list
static node *free_list[NUM_LEVEL] = {NULL};

int get_level(size_t size) {
    for (int i = 0; i < NUM_LEVEL; ++i) {
        if (size <= level_size[i]) {
            return i;
        }
    }
    return NUM_LEVEL - 1;
}

static void init_pool() {
    pool = mmap(NULL, 20000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (pool == MAP_FAILED) {
        pool = NULL;
        return;
    }
    first_malloc = 1;
    node *initial_node = (node *)pool;
    initial_node->start_addr = 0;
    initial_node->size = 20000;
    initial_node->free = 1;
    initial_node->next = NULL;
    initial_node->free_next = NULL;

    int level = get_level(initial_node->size);
    free_list[level] = initial_node;
}

static void *find_free_block(size_t size) {
    int start_lvl = get_level(size);

    for (int lvl = start_lvl; lvl < NUM_LEVEL; ++lvl) {
        node *cur = free_list[lvl];
        while (cur) {
            if (cur->size >= size) {
                return cur;
            }
            cur = cur->free_next;
        }
    }
    return NULL;
}

void insert_to_free_list(node *block) {
    int level = get_level(block->size);
    block->free_next = free_list[level];
    free_list[level] = block;
}

void remove_from_free_list(node *block) {
    int level = get_level(block->size);

    if (free_list[level] == block) {
        free_list[level] = block->free_next;
    } else {
        node *cur = free_list[level];
        while (cur && cur->free_next != block) {
            cur = cur->free_next;
        }
        if (cur) {
            cur->free_next = block->free_next;
        }
    }

    block->free_next = NULL;
}

static void *split_block(node *block, size_t size) {
    node *child = (node *)((char *)block + size); // the new free block
    child->start_addr = block->start_addr + size;
    child->size = block->size - size;
    child->free = 1;

    // update the linked list
    child->next = block->next;
    child->free_next = NULL;

    block->size = size;
    block->free = 0;
    block->next = child;

    // insert the new free block into free_list
    insert_to_free_list(child);

    return block;
}

int round_up(size_t size) {
    
    size_t rounded = (size + 31) & ~31;
    rounded += sizeof(node); // include header size
    return (int)rounded;
}

void *malloc(size_t size) {
    if (size == 0) {
        size_t max_free_size = 0;
        for (int i = NUM_LEVEL - 1; i >= 0; --i) {
            node *cur = free_list[i];
            while (cur) {
                if (cur->free && cur->size > max_free_size) {
                    max_free_size = cur->size;
                }
                cur = cur->free_next;
            }
            if (max_free_size > 0) break;
        }
        max_free_size -= sizeof(node); // without header size
        char buf[128];
        int n = snprintf((buf), sizeof(buf), "Max Free Chunk Size = %zu\n", max_free_size);
        if (n > 0) write(STDOUT_FILENO, buf, (size_t)n);

        if (pool) {
            munmap(pool, 20000);
            pool = NULL;
            first_malloc = 0;
            for (int i = 0; i < NUM_LEVEL; ++i) {
                free_list[i] = NULL;
            }
        }

        return NULL;
    }
    if (!first_malloc) init_pool();

    size_t total_size = round_up(size);

    node *best_fit = find_free_block(total_size);
    if (best_fit == NULL) {
        return NULL;
    }

    remove_from_free_list(best_fit);

    if (best_fit->size >= total_size + sizeof(node)) {
        best_fit = split_block(best_fit, total_size);
    } else {
        best_fit->free = 0;
    }

    return (void *)((char *)best_fit + sizeof(node));
}

void free(void *ptr) {
    if (ptr == NULL) return;

    node *block = (node *)((char *)ptr - sizeof(node));
    if (block->free) {
        fprintf(stderr, "Warning: Double free detected!\n");
        return;
    }

    block->free = 1;
    node *coalesced_block = block;
    node *prev_block = NULL;

    // find previous block
    node *current = (node *)pool;
    while (current && current->next != block) {
        current = current->next;
    }
    prev_block = current;

    if (prev_block && prev_block->free) {
        remove_from_free_list(prev_block);
        prev_block->size += block->size;
        prev_block->next = block->next;
        coalesced_block = prev_block;
    }

    // find next block
    node *next_block = coalesced_block->next;
    if (next_block && next_block->free) {
        remove_from_free_list(next_block);
        coalesced_block->size += next_block->size;
        coalesced_block->next = next_block->next;
    }

    insert_to_free_list(coalesced_block);
}