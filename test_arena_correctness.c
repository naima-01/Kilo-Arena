#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "arena.h"

void test_basic_allocation(void) {
    printf("Test 1: Basic allocation... ");
    arena_t *a = arena_create(1024);
    void *p1 = arena_alloc(a, 100);
    void *p2 = arena_alloc(a, 200);
    assert(p1 != NULL);
    assert(p2 != NULL);

    uintptr_t p1_end = (uintptr_t)p1 + 100;
    uintptr_t p2_start = (uintptr_t)p2;
    assert(p1_end <= p2_start);  // No overlap, p2 after p1
    arena_destroy(a);
    printf("PASSED\n");
}

void test_alignment(void) {
    printf("Test 2: Alignment... ");
    arena_t *a = arena_create(1024);
    void *p1 = arena_alloc_aligned(a, 10, 8);
    void *p2 = arena_alloc_aligned(a, 10, 16);
    void *p3 = arena_alloc_aligned(a, 10, 32);
    assert((uintptr_t)p1 % 8 == 0);
    assert((uintptr_t)p2 % 16 == 0);
    assert((uintptr_t)p3 % 32 == 0);
    
    uintptr_t p2_aligned = ((uintptr_t)p1 + 10 + 15) & ~15;
    assert((uintptr_t)p2 == p2_aligned);
    
    arena_destroy(a);
    printf("PASSED\n");
}

void test_reset_behavior(void) {
    printf("Test 3: Reset reuses memory... ");
    arena_t *a = arena_create(1024);
    void *p1 = arena_alloc(a, 500);
    size_t offset1 = a->offset;
    arena_reset(a);
    void *p2 = arena_alloc(a, 500);
    //offset should be back to 0 ish
    assert(p1 == p2);              
    assert(a->base + 0 == p2);     
    arena_destroy(a);
    printf("PASSED\n");
}

void test_arena_overflow(void) {
    printf("Test 4: Out of memory handling... ");
    arena_t *a = arena_create(100);
    void *p1 = arena_alloc(a, 60);
    void *p2 = arena_alloc(a, 60); 
    assert(p1 != NULL);
    assert(p2 == NULL);
    assert(a->stats.failed_allocations >= 1);
    arena_destroy(a);
    printf("PASSED\n");
}

void test_mixed_sizes(void) {
    printf("Test 5: Mixed allocation sizes (no overlap)... ");
    arena_t *a = arena_create(4096);
    int sizes[] = {8, 16, 32, 64, 128, 256, 512, 1024};
    void *ptrs[8];
    uintptr_t ends[8];
    
    for (int i = 0; i < 8; i++) {
        ptrs[i] = arena_alloc(a, sizes[i]);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xAA, sizes[i]);  // Write pattern
        ends[i] = (uintptr_t)ptrs[i] + sizes[i];
    }
    
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            uintptr_t start_i = (uintptr_t)ptrs[i];
            uintptr_t start_j = (uintptr_t)ptrs[j];
            uintptr_t end_i = ends[i];
            uintptr_t end_j = ends[j];

            int overlap = (start_i < end_j && start_j < end_i);
            assert(!overlap);
        }
    }
    arena_destroy(a);
    printf("PASSED\n");
}

void test_stats_tracking(void) {
    printf("Test 6: Statistics tracking... ");
    arena_t *a = arena_create(1024);
    
    assert(a->stats.allocation_count == 0);
    assert(a->stats.total_bytes_allocated == 0);
    
    arena_alloc(a, 100);
    assert(a->stats.allocation_count == 1);
    assert(a->stats.total_bytes_allocated == 100);
    
    arena_alloc(a, 200);
    assert(a->stats.allocation_count == 2);
    assert(a->stats.total_bytes_allocated == 300);
    
    arena_reset(a);
    assert(a->stats.reset_count == 1);
    assert(a->stats.current_arena_offset == 0);
    
    arena_destroy(a);
    printf("PASSED\n");
}

void test_arena_rewind(void) {
    printf("Test 7: Rewind to previous offset... ");
    arena_t *a = arena_create(1024);
    
    arena_alloc(a, 100);
    size_t offset1 = a->offset;
    arena_alloc(a, 100);
    size_t offset2 = a->offset;
    
    arena_rewind(a, offset1);
    assert(a->offset == offset1);
    
    //should be able to allocate again, overwriting previous data
    void *p = arena_alloc(a, 50);
    assert((uintptr_t)p + 50 <= (uintptr_t)a->base + offset2);
    
    arena_destroy(a);
    printf("PASSED\n");
}

int main(void) {
    printf("\n⋆˚꩜｡ Arena Correctness Tests ⋆˚꩜｡\n\n");
    test_basic_allocation();
    test_alignment();
    test_reset_behavior();
    test_arena_overflow();
    test_mixed_sizes();
    test_stats_tracking();
    test_arena_rewind();
    return 0;
}