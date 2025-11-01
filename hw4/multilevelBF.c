#include <stddef.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define NUM_LEVEL 11
static size_t level_size[NUM_LEVEL] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
// use mmap(-1) to create my own "heap" then do the bf

static void *pool = NULL;
static int first_malloc = 0; 



typedef struct Node {
    int start_addr; // offset from pool, the starter of the header
    size_t size; // include the header size
    int free;
    struct Node *next;
    struct Node *prev;
    uint32_t padding; // to make the header size 32 bytes
}node;
static node* free_list[NUM_LEVEL] = {NULL};

static int size_to_level(size_t total){
    for(int i = 0; i < NUM_LEVEL; ++i){
        if(total <= level_size[i]) return i;
    }
    return NUM_LEVEL - 1;
}

static void init_pool(){
    pool = mmap(NULL, 20000, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if(pool == MAP_FAILED){pool = NULL; return;}
    first_malloc = 1;
    node *initial_node = (node *)pool;
    initial_node->start_addr = 0;
    initial_node->size = 20000;
    initial_node->free = 1;
    initial_node->next = NULL;
    initial_node->prev = NULL;
    int level = NUM_LEVEL - 1; // largest level
    free_list[level] = initial_node;
}

static void *find_free_block(size_t size){
    node *best_fit = NULL;
    int start_lvl = size_to_level(size);
    for(int lvl = start_lvl; lvl < NUM_LEVEL; ++lvl){
        node *cur = free_list[lvl];
        while(cur){
            if(cur->size >= size){
                if(best_fit == NULL || cur->size < best_fit->size){
                    best_fit = cur;
                }
            }
            cur = cur->next;
        }
        if(best_fit) break; 
    }
    return best_fit;
}

static void update_free_list(){
    /* clear heads */
    for(int i = 0; i < NUM_LEVEL; ++i) free_list[i] = NULL;

    if(!pool) return;
    char *p = (char*)pool;
    char *end = p + 20000;

    node *cur = (node*)p;
    while((char*)cur < end){
        /* defensively check size to avoid infinite loop */
        if(cur->size == 0 || (char*)cur + cur->size > end){
            // corrupted size — stop to avoid infinite loop
            break;
        }

        /* reset links for safety */
        cur->next = cur->prev = NULL;

        if(cur->free){
            int lvl = size_to_level(cur->size);
            /* insert at head of level list */
            cur->next = free_list[lvl];
            if(free_list[lvl]) free_list[lvl]->prev = cur;
            free_list[lvl] = cur;
            cur->prev = NULL;
        }
        /* move to next chunk by size (size is total chunk size) */
        cur = (node *)((char*)cur + cur->size);
    }
    return;
}

static void *split_block(node *block, size_t size){
    // the original become new (taken) + old (free)
    node *original_next = block->next;
    node *original_prev = block->prev;
    node *child = (node *)((char *)block + size);// this is the new free block
    child->start_addr = block->start_addr + size;
    child->size = block->size - size;
    child->free = 1;
    child->next = original_next;
    child->prev = block;

    block->size = size;
    block->next = child;
    block->free = 0;
    if(original_next) original_next->prev = child;

    return block;
}

int round_up(size_t size){ // round the requested size to multiple of 32
    if(size % 32 == 0) return size;
    return (size / 32 + 1) * 32;
}

void *malloc(size_t size){
    write(STDOUT_FILENO, "malloc called\n", 14);
    if(size == 0){
        // print out the largest free chunk
        // use munmap to release the memory pool
        size_t max_free_size = 0;
        for(int i = NUM_LEVEL-1; i >=0; --i){
            node *cur = free_list[i];
            while(cur){
                if(cur->free && cur->size > max_free_size){
                    max_free_size = cur->size;
                }
                cur = cur->next;
            }
            if(max_free_size > 0) break;
        }

        char buf[128];
        int n = snprintf((buf), sizeof(buf), "Max Free Chunk Size = %zu", max_free_size);// calculate the size
        if(n > 0) write(STDOUT_FILENO, buf, (size_t)n);

        if(pool){
            munmap(pool, 20000);
            pool = NULL;
            first_malloc = 0;
            for(int i = 0; i < NUM_LEVEL; ++i) free_list[i] = NULL;
        }

        return NULL;
    }
    if(!first_malloc) init_pool();
    size = round_up(size);
    size += sizeof(node); // include the header size
    // implement best-fit algorithm to find the best chunk
    node *best_fit = find_free_block(size);
    if(best_fit == NULL){ return NULL; }
    best_fit = split_block(best_fit, size);
    update_free_list();
    return (void *)((char *)best_fit + sizeof(node));

}

void free(void *ptr){
    if(ptr == NULL) return;
    node *block = (node *)((char *)ptr - sizeof(node));
    block->free = 1;

    node *prev_block = block->prev;
    node *next_block = block->next;

    if(prev_block && prev_block->free){
        // merge with previous
        prev_block->size += block->size;
        prev_block->next = block->next;
    }
    if(next_block && next_block->free){
        // merge with next
        if(prev_block && prev_block->free){
            prev_block->size += next_block->size;
            prev_block->next = next_block->next;
        } else {
            block->size += next_block->size;
            block->next = next_block->next;
        }
    }
    update_free_list();
}

