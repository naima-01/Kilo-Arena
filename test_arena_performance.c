#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "arena.h"

#define TEST_SIZE (10 * 1024 * 1024) 
#define NUM_ALLOCS 100000

void test_malloc_performance(void) {
    clock_t start = clock();
    void *ptrs[NUM_ALLOCS];
    
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = malloc(100);
        if (ptrs[i]) memset(ptrs[i], 0, 100);
    }
    
    for (int i = 0; i < NUM_ALLOCS; i++) {
        free(ptrs[i]);
    }
    
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    printf("malloc/free: %f seconds for %d allocations\n", time_spent, NUM_ALLOCS);
}

void test_arena_performance(void) {
    arena_t *arena = arena_create(TEST_SIZE);
    clock_t start = clock();
    void *ptrs[NUM_ALLOCS];
    
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = arena_alloc(arena, 100);
        if (ptrs[i]) memset(ptrs[i], 0, 100);
    }
    
    arena_reset(arena);
    
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;
    printf("arena_alloc:   %f seconds for %d allocations\n", time_spent, NUM_ALLOCS);
    
    arena_destroy(arena);
}

int main(void) {
    printf("=== Performance Comparison ===\n");
    printf("Running %d allocations of 100 bytes each...\n\n", NUM_ALLOCS);
    
    test_malloc_performance();
    test_arena_performance();
    
    return 0;
}