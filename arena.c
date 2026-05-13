/* arena.c -- Arena allocator implementation */

#include "arena.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define ARENA_DEFAULT_ALIGNMENT (sizeof(void*))
#define ARENA_ALIGN_UP(value, alignment) \
    (((value) + (alignment) - 1) & ~((alignment) - 1))

/*new arena with the specified size*/
arena_t* arena_create(size_t size) {
    arena_t *arena = malloc(sizeof(arena_t));
    if (!arena) return NULL;
    
    uint8_t *memory = malloc(size);
    if (!memory) {
        free(arena);
        return NULL;
    }
    
    arena->base = memory;
    arena->size = size;
    arena->offset = 0;
    arena->peak_offset = 0;
    arena->owns_memory = true;
    
    memset(&arena->stats, 0, sizeof(arena_stats_t));
    
    return arena;
}

/*arena from pre-allocated memory */
arena_t* arena_create_from_memory(void *memory, size_t size) {
    arena_t *arena = malloc(sizeof(arena_t));
    if (!arena) return NULL;
    
    arena->base = (uint8_t*)memory;
    arena->size = size;
    arena->offset = 0;
    arena->peak_offset = 0;
    arena->owns_memory = false;
    
    memset(&arena->stats, 0, sizeof(arena_stats_t));
    
    return arena;
}

/* Destroy the arena and free associated memory */
void arena_destroy(arena_t *arena) {
    if (!arena) return;
    
    if (arena->owns_memory && arena->base) {
        free(arena->base);
    }
    free(arena);
}

/*allocate memory from arena*/
void* arena_alloc(arena_t *arena, size_t size) {
    return arena_alloc_aligned(arena, size, ARENA_DEFAULT_ALIGNMENT);
}

/*allocate aligned memory from arena*/
void* arena_alloc_aligned(arena_t *arena, size_t size, size_t alignment) {
    if (!arena || size == 0) return NULL;
    
    size_t current_offset = arena->offset;
    size_t aligned_offset = ARENA_ALIGN_UP(current_offset, alignment);
    size_t new_offset = aligned_offset + size;
    
    /* Check if there's enough space*/
    if (new_offset > arena->size) {
        arena->stats.failed_allocations++;
        return NULL;
    }
    
    void *ptr = arena->base + aligned_offset;
    arena->offset = new_offset;
    
    /*update tracking*/
    if (new_offset > arena->peak_offset) {
        arena->peak_offset = new_offset;
    }
    
    arena->stats.total_bytes_allocated += size;
    arena->stats.total_bytes_used += (new_offset - current_offset);
    arena->stats.current_arena_offset = arena->offset;
    arena->stats.peak_arena_offset = arena->peak_offset;
    arena->stats.allocation_count++;
    
    return ptr;
}

/*reset the arena, making all memory available again*/
void arena_reset(arena_t *arena) {
    if (!arena) return;
    
    arena->offset = 0;
    arena->stats.reset_count++;
    arena->stats.current_arena_offset = 0;
}

/*rewind the arena to a previous offset*/
void arena_rewind(arena_t *arena, size_t offset) {
    if (!arena || offset > arena->offset) return;
    
    arena->offset = offset;
    arena->stats.current_arena_offset = offset;
}

/*get remaining bytes in  arena*/
size_t arena_remaining(arena_t *arena) {
    if (!arena) return 0;
    return arena->size - arena->offset;
}

/*get used bytes in arena*/
size_t arena_used(arena_t *arena) {
    if (!arena) return 0;
    return arena->offset;
}

/*check if a pointer is within arena*/
bool arena_contains(arena_t *arena, void *ptr) {
    if (!arena || !ptr) return false;
    
    uint8_t *p = (uint8_t*)ptr;
    return (p >= arena->base && p < arena->base + arena->size);
}
