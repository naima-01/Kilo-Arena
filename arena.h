#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    size_t total_bytes_allocated;
    size_t total_bytes_used;
    size_t current_arena_offset;
    size_t peak_arena_offset;
    size_t allocation_count;
    size_t failed_allocations;
    size_t reset_count;
} arena_stats_t;

typedef struct {
    uint8_t *base;
    size_t size;
    size_t offset;
    size_t peak_offset;
    arena_stats_t stats;
    bool owns_memory;
} arena_t;

arena_t* arena_create(size_t size);
arena_t* arena_create_from_memory(void *memory, size_t size);
void arena_destroy(arena_t *arena);
void* arena_alloc(arena_t *arena, size_t size);
void* arena_alloc_aligned(arena_t *arena, size_t size, size_t alignment);
void arena_reset(arena_t *arena);
void arena_rewind(arena_t *arena, size_t offset);
size_t arena_remaining(arena_t *arena);
size_t arena_used(arena_t *arena);
bool arena_contains(arena_t *arena, void *ptr);

#endif