/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Heap implementation
 */

#include "jcontext.h"
#include "jmem.h"
#include "jrt-bit-fields.h"
#include "jrt-libc-includes.h"

#include "jmem-heap-segmented.h"
#define JMEM_ALLOCATOR_INTERNAL
#include "jmem-allocator-internal.h"
#include "jmem-heap-dynamic-emul-slab.h"
#include "jmem-profiler.h"

// Print GC occurance
// #define PRINT_GC_BEHAVIOR

/** \addtogroup mem Memory allocation
 * @{
 *
 * \addtogroup heap Heap
 * @{
 */

#ifndef JERRY_SYSTEM_ALLOCATOR
/**
 * Get end of region
 */
static inline jmem_heap_free_t *__attr_always_inline___ __attr_pure___
jmem_heap_get_region_end(jmem_heap_free_t *curr_p) /**< current region */
{
  return (jmem_heap_free_t *)((uint8_t *)curr_p + curr_p->size);
} /* jmem_heap_get_region_end */
#endif /* !JERRY_SYSTEM_ALLOCATOR */

#ifndef JERRY_ENABLE_EXTERNAL_CONTEXT
/**
 * Check size of heap is corresponding to configuration
 */
JERRY_STATIC_ASSERT(
    sizeof(jmem_heap_t) <= JMEM_HEAP_SIZE,
    size_of_mem_heap_must_be_less_than_or_equal_to_MEM_HEAP_SIZE);
#endif /* !JERRY_ENABLE_EXTERNAL_CONTEXT */

#ifdef JMEM_STATS

#ifdef JERRY_SYSTEM_ALLOCATOR
#error Memory statistics (JMEM_STATS) are not supported
#endif

static void jmem_heap_stat_init(void);
static void jmem_heap_stat_alloc(size_t num);
static void jmem_heap_stat_free(size_t num);
static void jmem_heap_stat_skip(void);
static void jmem_heap_stat_nonskip(void);
static void jmem_heap_stat_alloc_iter(void);
static void jmem_heap_stat_free_iter(void);

#define JMEM_HEAP_STAT_INIT() jmem_heap_stat_init()
#define JMEM_HEAP_STAT_ALLOC(v1) jmem_heap_stat_alloc(v1)
#define JMEM_HEAP_STAT_FREE(v1) jmem_heap_stat_free(v1)
#define JMEM_HEAP_STAT_SKIP() jmem_heap_stat_skip()
#define JMEM_HEAP_STAT_NONSKIP() jmem_heap_stat_nonskip()
#define JMEM_HEAP_STAT_ALLOC_ITER() jmem_heap_stat_alloc_iter()
#define JMEM_HEAP_STAT_FREE_ITER() jmem_heap_stat_free_iter()
#else /* !JMEM_STATS */
#define JMEM_HEAP_STAT_INIT()
#define JMEM_HEAP_STAT_ALLOC(v1)
#define JMEM_HEAP_STAT_FREE(v1)
#define JMEM_HEAP_STAT_SKIP()
#define JMEM_HEAP_STAT_NONSKIP()
#define JMEM_HEAP_STAT_ALLOC_ITER()
#define JMEM_HEAP_STAT_FREE_ITER()
#endif /* JMEM_STATS */

static inline void jmem_heap_print_allocator_type(void) {
/* Print allocator type */
  printf("\nIoT.js Memory Optimization Options\n");
  printf(">> Maximum JavaScript heap size: %dKB (%dB)\n",
    (JMEM_HEAP_AREA_SIZE / 1024), JMEM_HEAP_AREA_SIZE);

// Addressing
#if defined(JERRY_CPOINTER_32_BIT) \
    || defined(SEG_FULLBIT_ADDRESS_ALLOC) \
    || defined(JMEM_DYNAMIC_HEAP_EMUL)
  printf(">> Addressing: Full-bitwidth\n");
#elif defined(JMEM_SEGMENTED_HEAP)
  printf(">> Addressing: Multiple base compressed (MBCA)\n");
#else
  printf(">> Addressing: Single base compressed (SBCA)\n");
#endif

// Allocator type
#if defined(JERRY_SYSTEM_ALLOCATOR)
  printf(">> Allocator: dynamic object allocation\n");
#elif defined(JMEM_SEGMENTED_HEAP)
  printf(">> Allocator: dynamic segment allocation (DSA)\n");
#elif defined(JMEM_DYNAMIC_HEAP_EMUL)
  printf(">> Allocator: emulated dynamic object allocation\n");
#else
  printf(">> Allocator: static heap reservation\n");
#endif

  // Allocator detailed: 
#if defined(JERRY_SYSTEM_ALLOCATOR)
  // Allocator 1) Dynamic object allocation
#elif defined(JMEM_SEGMENTED_HEAP) /* defined(JERRY_SYSTEM_ALLOCATOR) */
  // Allocator 2) Dynamic segment allocation
  printf(">>>> Segment size: %dB\n", SEG_SEGMENT_SIZE);
  printf(">>>> Max segment count: %d\n", SEG_NUM_SEGMENTS);

  // MBCAT Fast path
#if defined(SEG_RMAP_CACHE)
  printf(">>>> MBCAT Fast path: reverse map cache (RMC)\n");
#if SEG_RMAP_CACHE_SET_SIZE == 1
  printf(">>>>>> Direct-mapped, cache size: %d\n", SEG_RMAP_CACHE_SIZE);
#elif SEG_RMAP_CACHE_SIZE == SEG_RMAP_CACHE_SET_SIZE
  printf(">>>>>> Fully-associative, cache size: %d\n", SEG_RMAP_CACHE_SIZE);
#elif SEG_RMAP_CACHE_SIZE > SEG_RMAP_CACHE_SET_SIZE
  printf(">>>>>> %d-way associative, cache size: %d, set size: %d\n",
         SEG_RMAP_CACHE_WAYS, SEG_RMAP_CACHE_SIZE, SEG_RMAP_CACHE_SET_SIZE);
#else
  printf(">>>>>> Invalid RMC setting, cache size: %d, set size: %d\n",
         SEG_RMAP_CACHE_SIZE, SEG_RMAP_CACHE_SET_SIZE);
#endif /* SEG_RMAP_CACHE_SET_SIZE, SEG_RMAP_CACHE_SET_SIZE */
#else  /* defined(SEG_RMAP_CACHE) */
  printf(">>>> MBCAT Fast path: none\n");
#endif /* !defined(SEG_RMAP_CACHE) */

  // MBCAT Slow path
#if defined(SEG_RMAP_BINSEARCH)
  printf(">>>> MBCAT Slow path: binary search based on reverse map tree (RMT)\n");
#elif defined(SEG_RMAP_2LEVEL_SEARCH)
  printf(">>>> MBCAT Slow path: 2-level search (FIFO cache size: %d)\n",
         SEG_RMAP_2LEVEL_SEARCH_FIFO_CACHE_SIZE);
#else
  printf(">>>> MBCAT Slow path: linear search based on segment base table\n");
#endif

#elif defined(JMEM_DYNAMIC_HEAP_EMUL) /* JMEM_SEGMENTED_HEAP */
  // Allocator 3) Emulated dynamic object allocation
#if defined(DE_SLAB)
  printf(">>>> Slab enabled\n");
#endif
#else /* JMEM_DYNAMIC_HEAP_EMUL */
  // Allocator 4) Static heap reservation
#endif
}

static inline void jmem_heap_init_first_free_region(void) {
  // Initialize first free region
#ifdef JMEM_SEGMENTED_HEAP
  jmem_heap_free_t *const region_p =
      (jmem_heap_free_t *)(JERRY_HEAP_CONTEXT(area[0]) + JMEM_ALIGNMENT);
  region_p->size = SEG_SEGMENT_SIZE - JMEM_ALIGNMENT;
#else
  jmem_heap_free_t *const region_p =
      (jmem_heap_free_t *)JERRY_HEAP_CONTEXT(area);
  region_p->size = JMEM_HEAP_AREA_SIZE;
#endif
  region_p->next_offset = JMEM_HEAP_END_OF_LIST;

  // Initialize leading free region
  JERRY_HEAP_CONTEXT(first).size = 0;
  JERRY_HEAP_CONTEXT(first).next_offset =
      JMEM_COMPRESS_POINTER_INTERNAL(region_p);
#ifdef PROF_COUNT__COMPRESSION_CALLERS
  profile_inc_count_compression_callers(1); // compression callers
#endif
  JERRY_CONTEXT(jmem_heap_list_skip_p) = &JERRY_HEAP_CONTEXT(first);
}

static inline void jmem_heap_init_size_metrics(void) {
  /* Calculate statically allocated size */
#if defined(JMEM_STATIC_HEAP)
  JERRY_CONTEXT(jmem_allocated_heap_size) = JMEM_HEAP_SIZE;
#endif
#if defined(JMEM_SEGMENTED_HEAP)
  JERRY_CONTEXT(jmem_segment_allocator_metadata_size) =
      SEG_NUM_SEGMENTS * SEG_METADATA_SIZE_PER_SEGMENT;
#endif
}

/**
 * Startup initialization of heap
 */
void jmem_heap_init(void) {
  // Check initial conditions
#ifndef JERRY_CPOINTER_32_BIT
  // the maximum heap size for 16bit compressed pointers should be 512K
  JERRY_ASSERT(((UINT16_MAX + 1) << JMEM_ALIGNMENT_LOG) >= JMEM_HEAP_SIZE);
#endif /* !defined(JERRY_CPOINTER_32_BIT) */
#ifndef JERRY_SYSTEM_ALLOCATOR
  JERRY_ASSERT((uintptr_t)JERRY_HEAP_CONTEXT(area) % JMEM_ALIGNMENT == 0);
#endif /* !defined(JERRY_SYSTEM_ALLOCATOR) */
  jmem_heap_print_allocator_type();

#if !defined(JERRY_SYSTEM_ALLOCATOR)
#if defined(JMEM_SEGMENTED_HEAP)
  init_segmented_heap();
#endif /* defined(JMEM_SEGMENTED_HEAP) */
  JERRY_CONTEXT(jmem_heap_limit) = CONFIG_MEM_HEAP_DESIRED_LIMIT;
  jmem_heap_init_first_free_region();
  jmem_heap_init_size_metrics();
#endif /* !defined(JERRY_SYSTEM_ALLOCATOR) */

  init_profilers(); /* Initialize profilers */
  JMEM_HEAP_STAT_INIT();
} /* jmem_heap_init */

/**
 * Finalize heap
 */
void jmem_heap_finalize(void) {
  finalize_profilers(); /* Finalize profilers */

#ifdef JMEM_SEGMENTED_HEAP
  free_empty_segment_groups();
  free_initial_segment_group();
  JERRY_ASSERT(JERRY_HEAP_CONTEXT(segments_count) == 0);
#endif
  JERRY_ASSERT(JERRY_CONTEXT(jmem_heap_blocks_size) == 0);
} /* jmem_heap_finalize */

static inline void *jmem_heap_alloc_block_internal_fast(bool is_small_block) {
  /* Fast path for 8B blocks, first region is guaranteed to be sufficient. */
  jmem_heap_free_t *data_space_p = NULL;
  // Minimal size (8B in compressed address / 16B in full-bitwidth address)
  data_space_p =
      JMEM_DECOMPRESS_POINTER_INTERNAL(JERRY_HEAP_CONTEXT(first).next_offset);
  // JERRY_ASSERT(jmem_is_heap_pointer(data_space_p));

  // Update heap blocks size
  JERRY_CONTEXT(jmem_heap_blocks_size) += JMEM_ALIGNMENT;
  JERRY_CONTEXT(jmem_heap_allocated_blocks_count)++;

  // Update allocated heap size, sys-alloc. metadata size (dynamic heap)
#if defined(JMEM_DYNAMIC_HEAP_EMUL)
#if defined(DE_SLAB)
  // Dynamic heap emulation with slab
  if (!is_small_block) {
    JERRY_CONTEXT(jmem_allocated_heap_size) += JMEM_ALIGNMENT;
    JERRY_CONTEXT(jmem_system_allocator_metadata_size) +=
        SYSTEM_ALLOCATOR_METADATA_SIZE;
  }
#else
  // Dynamic heap emulation without slab
  JERRY_CONTEXT(jmem_allocated_heap_size) += JMEM_ALIGNMENT;
  JERRY_CONTEXT(jmem_system_allocator_metadata_size) +=
      SYSTEM_ALLOCATOR_METADATA_SIZE;
  JERRY_UNUSED(is_small_block);
#endif /* !defined(DE_SLAB) */
#else
  JERRY_UNUSED(is_small_block);
#endif /* defined(JMEM_DYNAMIC_HEPA_EMUL) */

  uint32_t block_offset = JERRY_HEAP_CONTEXT(first).next_offset;
#ifdef JMEM_SEGMENTED_HEAP
  // Update segment occupied size (segment heap)
  uint32_t sidx = block_offset / SEG_SEGMENT_SIZE;
  jmem_segment_t *segment_header = &JERRY_HEAP_CONTEXT(segments[sidx]);
  segment_header->occupied_size += JMEM_ALIGNMENT;
  // JERRY_ASSERT(segment_header->occupied_size <= SEG_SEGMENT_SIZE);
#endif /* defined(JMEM_SEGMENTED_HEAP) */
  JMEM_HEAP_STAT_ALLOC_ITER();

  // Update free region metadata
  if (data_space_p->size == JMEM_ALIGNMENT) {
    JERRY_HEAP_CONTEXT(first).next_offset = data_space_p->next_offset;
  } else {
    // JERRY_ASSERT(data_space_p->size > JMEM_ALIGNMENT);
    uint32_t remaining_offset = block_offset + JMEM_ALIGNMENT;
    jmem_heap_free_t *remaining_p =
        JMEM_DECOMPRESS_POINTER_INTERNAL(remaining_offset);
    remaining_p->size = data_space_p->size - JMEM_ALIGNMENT;
    remaining_p->next_offset = data_space_p->next_offset;
    JERRY_HEAP_CONTEXT(first).next_offset = remaining_offset;
  }

  // Update fast path skipping pointer
  if (unlikely(data_space_p == JERRY_CONTEXT(jmem_heap_list_skip_p))) {
    JERRY_CONTEXT(jmem_heap_list_skip_p) =
        JMEM_DECOMPRESS_POINTER_INTERNAL(JERRY_HEAP_CONTEXT(first).next_offset);
  }
  return data_space_p;
}

static inline void *jmem_heap_alloc_block_internal_slow(
    const size_t required_size, bool is_small_block) {
  /* Slow path for larger regions. */
  jmem_heap_free_t *data_space_p = NULL;
  uint32_t current_offset = JERRY_HEAP_CONTEXT(first).next_offset;
  jmem_heap_free_t *prev_p = &JERRY_HEAP_CONTEXT(first);
  while (current_offset != JMEM_HEAP_END_OF_LIST) {
    jmem_heap_free_t *current_p =
        JMEM_DECOMPRESS_POINTER_INTERNAL(current_offset);
    // JERRY_ASSERT(jmem_is_heap_pointer(current_p));
    JMEM_HEAP_STAT_ALLOC_ITER();

    const uint32_t next_offset = current_p->next_offset;
// #ifdef JMEM_SEGMENTED_HEAP
//     jmem_heap_free_t *next_p = JMEM_DECOMPRESS_POINTER_INTERNAL(next_offset);
//     JERRY_ASSERT(next_offset == JMEM_HEAP_END_OF_LIST_UINT32 ||
//                  jmem_is_heap_pointer(next_p));
// #else
//     JERRY_ASSERT(
//         next_offset == JMEM_HEAP_END_OF_LIST ||
//         jmem_is_heap_pointer(JMEM_DECOMPRESS_POINTER_INTERNAL(next_offset)));
// #endif

    if (current_p->size >= required_size) {
      /* Region is sufficiently big, store address. */
      data_space_p = current_p;

      // Update heap blocks size
      JERRY_CONTEXT(jmem_heap_blocks_size) += required_size;
      JERRY_CONTEXT(jmem_heap_allocated_blocks_count)++;
      // Update allocated heap size, sys-alloc. metadata size (dynamic heap)
#if defined(JMEM_DYNAMIC_HEAP_EMUL)
#if defined(DE_SLAB)
      // Dynamic heap emulation with slab
      if (!is_small_block) {
        JERRY_CONTEXT(jmem_allocated_heap_size) += required_size;
        JERRY_CONTEXT(jmem_system_allocator_metadata_size) +=
            SYSTEM_ALLOCATOR_METADATA_SIZE;
      }
#else
      // Dynamic heap emulation without slab
      JERRY_CONTEXT(jmem_allocated_heap_size) += required_size;
      JERRY_CONTEXT(jmem_system_allocator_metadata_size) +=
          SYSTEM_ALLOCATOR_METADATA_SIZE;
      JERRY_UNUSED(is_small_block);
#endif /* !defined(DE_SLAB) */
#else
      JERRY_UNUSED(is_small_block);
#endif /* defined(JMEM_DYNAMIC_HEPA_EMUL) */

      // Update segment occupied size (segment heap)
#ifdef JMEM_SEGMENTED_HEAP
      int size_to_alloc = (int)required_size;
      uint32_t block_start_offset = current_offset;
      uint32_t block_end_offset =
          block_start_offset + (uint32_t)size_to_alloc - JMEM_ALIGNMENT;
      uint32_t fragment_start_offset = block_start_offset;
      while (size_to_alloc > 0) {
        uint32_t sidx = fragment_start_offset / SEG_SEGMENT_SIZE;
        uint32_t segment_end_offset =
            (sidx + 1) * SEG_SEGMENT_SIZE - JMEM_ALIGNMENT;
        uint32_t fragment_end_offset;
        if (block_end_offset < segment_end_offset) {
          fragment_end_offset = block_end_offset;
        } else {
          fragment_end_offset = segment_end_offset;
        }
        uint32_t size_to_alloc_in_segment =
            fragment_end_offset - fragment_start_offset + JMEM_ALIGNMENT;

        jmem_segment_t *segment_header = &JERRY_HEAP_CONTEXT(segments[sidx]);
        segment_header->occupied_size += size_to_alloc_in_segment;
        // JERRY_ASSERT(segment_header->occupied_size <= SEG_SEGMENT_SIZE);
        size_to_alloc -= (int)size_to_alloc_in_segment;
        // JERRY_ASSERT(size_to_alloc >= 0);
        fragment_start_offset = fragment_end_offset + JMEM_ALIGNMENT;
      }
#endif

      /* Region was larger than necessary. */
      if (current_p->size > required_size) {
        /* Get address of remaining space. */
        jmem_heap_free_t *const remaining_p =
            (jmem_heap_free_t *)((uint8_t *)current_p + required_size);

        /* Update metadata. */
        remaining_p->size = current_p->size - (uint32_t)required_size;
        remaining_p->next_offset = next_offset;

        /* Update list. */
#ifdef JMEM_SEGMENTED_HEAP
        prev_p->next_offset = current_offset + (uint32_t)required_size;
#else
        prev_p->next_offset = JMEM_COMPRESS_POINTER_INTERNAL(remaining_p);
#endif
      }
      /* Block is an exact fit. */
      else {
        /* Remove the region from the list. */
        prev_p->next_offset = next_offset;
      }

      JERRY_CONTEXT(jmem_heap_list_skip_p) = prev_p;

      /* Found enough space. */
      break;
    }

    /* Next in list. */
    prev_p = current_p;
    current_offset = next_offset;
  }
  return data_space_p;
}

static inline void *jmem_heap_alloc_block_internal_dynamic_real(
    const size_t size, bool is_small_block) {
  void *data_space_p = malloc(size);
  // Dynamic heap
  size_t aligned_size = size + SYSTEM_ALLOCATOR_METADATA_SIZE;
  aligned_size = ((aligned_size + SYSTEM_ALLOCATOR_ALIGN_BYTES - 1) /
                  SYSTEM_ALLOCATOR_ALIGN_BYTES) *
                 SYSTEM_ALLOCATOR_ALIGN_BYTES;

  JERRY_CONTEXT(jmem_heap_blocks_size) += size;
  JERRY_CONTEXT(jmem_allocated_heap_size) += aligned_size;
  JERRY_CONTEXT(jmem_system_allocator_metadata_size) +=
      SYSTEM_ALLOCATOR_METADATA_SIZE;
  JERRY_CONTEXT(jmem_heap_allocated_blocks_count)++;
  JERRY_UNUSED(is_small_block);
  return data_space_p;
}

/**
 * Allocation of memory region.
 *
 * See also:
 *          jmem_heap_alloc_block
 *
 * @return pointer to allocated memory block - if allocation is successful,
 *         NULL - if there is not enough memory.
 */
static __attr_hot___ void *jmem_heap_alloc_block_internal(const size_t size,
                                                          bool is_small_block) {
  profile_alloc_start(); /* Time profiling */

#ifndef JERRY_SYSTEM_ALLOCATOR
  // Align size
  const size_t required_size =
      ((size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT) * JMEM_ALIGNMENT;
  // Try to allocate block
  jmem_heap_free_t *data_space_p = NULL;
  if (required_size == JMEM_ALIGNMENT &&
      likely(JERRY_HEAP_CONTEXT(first).next_offset != JMEM_HEAP_END_OF_LIST)) {
    data_space_p = jmem_heap_alloc_block_internal_fast(is_small_block);
  } else {
    data_space_p =
        jmem_heap_alloc_block_internal_slow(required_size, is_small_block);
  }

  // legacy code: jmem_heap_init (it does not contribute to jmem-heap behaviors)
  while (JERRY_CONTEXT(jmem_heap_blocks_size) >=
         JERRY_CONTEXT(jmem_heap_limit)) {
    JERRY_CONTEXT(jmem_heap_limit) += CONFIG_MEM_HEAP_DESIRED_LIMIT;
  }
  if (unlikely(!data_space_p)) {
    // In case of block allocation failure
    profile_alloc_end(); /* Time profiling */
    return NULL;
  }
  // JERRY_ASSERT((uintptr_t)data_space_p % JMEM_ALIGNMENT == 0);
  JMEM_HEAP_STAT_ALLOC(size);
  profile_alloc_end(); /* Time profiling */
  return (void *)data_space_p;
#else  /* JERRY_SYSTEM_ALLOCATOR */
  void *data_space_p =
      jmem_heap_alloc_block_internal_dynamic_real(size, is_small_block);
  profile_alloc_end(); /* Time profiling */
  return data_space_p;
#endif /* !JERRY_SYSTEM_ALLOCATOR */
} /* jmem_heap_alloc_block_internal */

/**
 * Allocation of memory block, running 'try to give memory back' callbacks, if
 * there is not enough memory.
 *
 * Note:
 *      if there is still not enough memory after running the callbacks
 *        - NULL value will be returned if parmeter 'ret_null_on_error' is true
 *        - the engine will terminate with ERR_OUT_OF_MEMORY if
 * 'ret_null_on_error' is false
 *
 * @return NULL, if the required memory size is 0
 *         also NULL, if 'ret_null_on_error' is true and the allocation fails
 * because of there is not enough memory
 */
static void *jmem_heap_gc_and_alloc_block(
    const size_t required_size, /**< required memory size */
    bool ret_null_on_error,     /**< indicates whether return null or terminate
                                     with ERR_OUT_OF_MEMORY on out of memory */
    bool is_small_block)        /**< is small JSObject or not */
{
  if (unlikely(required_size == 0)) {
    return NULL;
  }
  size_t size =
      (required_size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;

#ifdef JMEM_GC_BEFORE_EACH_ALLOC
  // GC before each alloc: not enabled in most cases
#ifdef PRINT_GC_BEHAVIOR
  printf("GC 0: before each alloc\n");
#endif
  jmem_run_free_unused_memory_callbacks(JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH);
#endif /* JMEM_GC_BEFORE_EACH_ALLOC */

  // Call GC if free memory is expected to lack
#if defined(JMEM_STATIC_HEAP) || defined(JMEM_SEGMENTED_HEAP)
  size_t allocated_size = JERRY_CONTEXT(jmem_heap_blocks_size) + size;
#else /* defined(JMEM_STATIC_HEAP) || defined(JMEM_SEGMENTED_HEAP) */
  size_t allocated_size = JERRY_CONTEXT(jmem_allocated_heap_size) + size;
#if defined(DE_SLAB)
  if (is_small_block)
    allocated_size -= size;
#endif /* defined(DE_SLAB) */
#endif /* !defined(JMEM_STATIC_HEAP) && !defined(JMEM_SEGMENTED_HEAP) */

#ifdef JMEM_LAZY_GC
  if (allocated_size > JMEM_HEAP_SIZE) {
#else
  if (allocated_size > JERRY_CONTEXT(jmem_heap_limit)) {
#endif
#ifdef PRINT_GC_BEHAVIOR
    printf("GC 1: expected over-size\n");
#endif
    print_segment_utiliaztion_profile_before_gc(size); /* Seg-util profiling */
    jmem_run_free_unused_memory_callbacks(JMEM_FREE_UNUSED_MEMORY_SEVERITY_LOW);
    print_segment_utiliaztion_profile_after_gc(size); /* Seg-util profiling */
  }
  void *data_space_p =
      jmem_heap_alloc_block_internal(size, is_small_block); // BLOCK ALLOC
  if (likely(data_space_p != NULL)) {
    print_total_size_profile_on_alloc();   /* Total size profiling */
    profile_jsobject_inc_allocation(size); /* JS object allocation profiling */
    return data_space_p;
  }
  // Segment Allocation before GC
#ifdef JMEM_SEGMENTED_HEAP
  /* Segment utilization profiling */
  print_segment_utilization_profile_before_segalloc(size);
  if (alloc_a_segment_group(size) != NULL) {
    data_space_p =
        jmem_heap_alloc_block_internal(size, is_small_block); // BLOCK ALLOC
    // JERRY_ASSERT(data_space_p != NULL);
    return data_space_p;
  }
#endif /* JMEM_SEGMENTED_HEAP */
  for (jmem_free_unused_memory_severity_t severity =
           JMEM_FREE_UNUSED_MEMORY_SEVERITY_LOW;
       severity <= JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH;
       severity = (jmem_free_unused_memory_severity_t)(severity + 1)) {
#ifdef PRINT_GC_BEHAVIOR
    printf("GC 2: failed due to fragmentation. retry to GC (severity=%d)\n",
           (int)severity);
#endif
    /* Garbage collection -> try to alloc a block */
    print_segment_utiliaztion_profile_before_gc(
        size); /* Segment utilization profiling */
    jmem_run_free_unused_memory_callbacks(severity);
    print_segment_utiliaztion_profile_after_gc(
        size); /* Segment utilization profiling */
    data_space_p =
        jmem_heap_alloc_block_internal(size, is_small_block); // BLOCK ALLOC
    if (likely(data_space_p != NULL)) {
      print_total_size_profile_on_alloc(); /* Total size profiling */
      profile_jsobject_inc_allocation(
          size); /* JS object allocation profiling */
      return data_space_p;
    }
  }
  // Segment allocation after GC
#ifdef JMEM_SEGMENTED_HEAP
  /* Segment utilization profiling */
  print_segment_utilization_profile_before_segalloc(size);
  if (alloc_a_segment_group(size) != NULL) {
    data_space_p =
        jmem_heap_alloc_block_internal(size, is_small_block); // BLOCK ALLOC
    // JERRY_ASSERT(data_space_p != NULL);
    return data_space_p;
  }
#endif /* JMEM_SEGMENTED_HEAP */
  // JERRY_ASSERT(data_space_p == NULL);

  if (!ret_null_on_error) {
    jerry_fatal(ERR_OUT_OF_MEMORY);
  }
  return data_space_p;
} /* jmem_heap_gc_and_alloc_block */

/**
 * Allocation of memory block, running 'try to give memory back' callbacks, if
 * there is not enough memory.
 *
 * Note:
 *      If there is still not enough memory after running the callbacks, then
 * the engine will be terminated with ERR_OUT_OF_MEMORY.
 *
 * @return NULL, if the required memory is 0
 *         pointer to allocated memory block, otherwise
 */
inline void *__attr_hot___ __attr_always_inline___
jmem_heap_alloc_block(const size_t size) /**< required memory size */
{
  void *ret = jmem_heap_gc_and_alloc_block(size, false, false);
  return ret;
} /* jmem_heap_alloc_block */

/**
 * Allocation of memory block, running 'try to give memory back' callbacks, if
 * there is not enough memory.
 *
 * Note:
 *      If there is still not enough memory after running the callbacks, NULL
 * will be returned.
 *
 * @return NULL, if the required memory size is 0
 *         also NULL, if the allocation has failed
 *         pointer to the allocated memory block, otherwise
 */
inline void *__attr_hot___ __attr_always_inline___
jmem_heap_alloc_block_null_on_error(
    const size_t size) /**< required memory size */
{
  void *ret = jmem_heap_gc_and_alloc_block(size, true, false);
  return ret;
} /* jmem_heap_alloc_block_null_on_error */

/**
 * Free the memory block.
 */
static void __attr_hot___ jmem_heap_free_block_internal(
    void *ptr,         /**< pointer to beginning of data space of the block */
    const size_t size, /**< size of allocated region */
    bool is_small_object) /**< is small object or not */
{
#ifndef JERRY_SYSTEM_ALLOCATOR
  profile_free_start(); /* Time profiling */

  /* checking that ptr points to the heap */
  JERRY_ASSERT(jmem_is_heap_pointer(ptr));
  JERRY_ASSERT(size > 0);
  JERRY_ASSERT(JERRY_CONTEXT(jmem_heap_limit) >=
               JERRY_CONTEXT(jmem_heap_blocks_size));

  JMEM_HEAP_STAT_FREE_ITER();

  jmem_heap_free_t *block_p = (jmem_heap_free_t *)ptr;
  jmem_heap_free_t *prev_p;
  jmem_heap_free_t *next_p;
  uint32_t next_cp;

#ifdef JMEM_SEGMENTED_HEAP
  uint32_t boffset = JMEM_COMPRESS_POINTER_INTERNAL(block_p);
#ifdef PROF_COUNT__COMPRESSION_CALLERS
  profile_inc_count_compression_callers(1); // compression callers
#endif
  uint32_t skip_offset =
      JMEM_COMPRESS_POINTER_INTERNAL(JERRY_CONTEXT(jmem_heap_list_skip_p));
#ifdef PROF_COUNT__COMPRESSION_CALLERS
  profile_inc_count_compression_callers(1); // compression callers
#endif
  bool is_skip_ok = boffset > skip_offset;
#else  /* JMEM_SEGMENTED_HEAP */
  bool is_skip_ok = block_p > JERRY_CONTEXT(jmem_heap_list_skip_p);
#endif /* !JMEM_SEGMENTED_HEAP */
  if (is_skip_ok) {
    prev_p = JERRY_CONTEXT(jmem_heap_list_skip_p);
    JMEM_HEAP_STAT_SKIP();
  } else {
    prev_p = &JERRY_HEAP_CONTEXT(first);
    JMEM_HEAP_STAT_NONSKIP();
  }

  // JERRY_ASSERT(jmem_is_heap_pointer(block_p));
#ifdef JMEM_SEGMENTED_HEAP
  const uint32_t block_offset = boffset;
#else  /* JMEM_SEGMENTED_HEAP */
  const uint32_t block_offset = JMEM_COMPRESS_POINTER_INTERNAL(block_p);
#endif /* !JMEM_SEGMENTED_HEAP */

  /* Find position of region in the list. */
  while (prev_p->next_offset < block_offset) {
    next_cp = prev_p->next_offset;
    next_p = JMEM_DECOMPRESS_POINTER_INTERNAL(next_cp);
    // JERRY_ASSERT(jmem_is_heap_pointer(next_p));

    prev_p = next_p;

    JMEM_HEAP_STAT_FREE_ITER();
  }

  next_cp = prev_p->next_offset;
  next_p = JMEM_DECOMPRESS_POINTER_INTERNAL(next_cp);

  /* Realign size */
  const size_t aligned_size =
      (size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;

  /* Update prev. */
  if (jmem_heap_get_region_end(prev_p) == block_p) {
    /* Can be merged. */
    prev_p->size += (uint32_t)aligned_size;
    block_p = prev_p;
  } else {
    block_p->size = (uint32_t)aligned_size;
    prev_p->next_offset = block_offset;
  }

  /* Update next. */
  if (jmem_heap_get_region_end(block_p) == next_p) {
    /* Can be merged. */
    block_p->size += next_p->size;
    block_p->next_offset = next_p->next_offset;
  } else {
    block_p->next_offset = next_cp;
  }

  JERRY_CONTEXT(jmem_heap_list_skip_p) = prev_p;

#ifdef JMEM_SEGMENTED_HEAP
  int size_to_free = (int)aligned_size;
  uint32_t block_start_offset = block_offset;
  uint32_t block_end_offset =
      block_start_offset + (uint32_t)size_to_free - JMEM_ALIGNMENT;
  uint32_t fragment_start_offset = block_start_offset;
  while (size_to_free > 0) {
    uint32_t sidx = fragment_start_offset / SEG_SEGMENT_SIZE;
    uint32_t segment_end_offset =
        (sidx + 1) * SEG_SEGMENT_SIZE - JMEM_ALIGNMENT;
    uint32_t fragment_end_offset;
    if (block_end_offset < segment_end_offset) {
      fragment_end_offset = block_end_offset;
    } else {
      fragment_end_offset = segment_end_offset;
    }
    uint32_t size_to_free_in_segment =
        fragment_end_offset - fragment_start_offset + JMEM_ALIGNMENT;

    jmem_segment_t *segment_header = &JERRY_HEAP_CONTEXT(segments[sidx]);
    segment_header->occupied_size -= size_to_free_in_segment;
    // JERRY_ASSERT(segment_header->occupied_size <= SEG_SEGMENT_SIZE);
    size_to_free -= (int)size_to_free_in_segment;
    // JERRY_ASSERT(size_to_free >= 0);
    fragment_start_offset = fragment_end_offset + JMEM_ALIGNMENT;
  }
#endif

  // Static heap or segmented heap
  // JERRY_ASSERT(JERRY_CONTEXT(jmem_heap_blocks_size) > 0);

  // Update heap blocks size
  JERRY_CONTEXT(jmem_heap_blocks_size) -= aligned_size;
  JERRY_CONTEXT(jmem_heap_allocated_blocks_count)--;

  // Update allocated heap size and sys-alloc. metadata size (dynamic heap)
#if defined(JMEM_DYNAMIC_HEAP_EMUL)
#if defined(DE_SLAB)
  // Dynamic heap with slab
  if (!is_small_object) {
    JERRY_CONTEXT(jmem_allocated_heap_size) -= aligned_size;
    JERRY_CONTEXT(jmem_system_allocator_metadata_size) -=
        SYSTEM_ALLOCATOR_METADATA_SIZE;
  }
#else  /* defined(DE_SLAB) */
  // Dynamic heap without slab
  JERRY_CONTEXT(jmem_allocated_heap_size) -= aligned_size;
  JERRY_CONTEXT(jmem_system_allocator_metadata_size) -=
      SYSTEM_ALLOCATOR_METADATA_SIZE;
  JERRY_UNUSED(is_small_object);
#endif /* !defined(DE_SLAB) */
#else
  JERRY_UNUSED(is_small_object);
#endif /* defined(JMEM_DYNAMIC_HEAP_EMUL) */

  while (JERRY_CONTEXT(jmem_heap_blocks_size) + CONFIG_MEM_HEAP_DESIRED_LIMIT <=
         JERRY_CONTEXT(jmem_heap_limit)) {
    JERRY_CONTEXT(jmem_heap_limit) -= CONFIG_MEM_HEAP_DESIRED_LIMIT;
  }

  // JERRY_ASSERT(JERRY_CONTEXT(jmem_heap_limit) >=
  //              JERRY_CONTEXT(jmem_heap_blocks_size));
  JMEM_HEAP_STAT_FREE(size);

  print_total_size_profile_on_alloc();                /* Total size profiling */
  print_segment_utilization_profile_after_free(size); /* Segment
                                                    utilization profiling */

  profile_free_end(); /* Time profiling */

#else  /* JERRY_SYSTEM_ALLOCATOR */
  JERRY_UNUSED(is_small_object);
  free(ptr);

  // Dynamic heap
  size_t aligned_size = size + SYSTEM_ALLOCATOR_METADATA_SIZE;
  aligned_size = ((aligned_size + SYSTEM_ALLOCATOR_ALIGN_BYTES - 1) /
                  SYSTEM_ALLOCATOR_ALIGN_BYTES) *
                 SYSTEM_ALLOCATOR_ALIGN_BYTES;
  JERRY_CONTEXT(jmem_heap_blocks_size) -= size;
  JERRY_CONTEXT(jmem_allocated_heap_size) -= aligned_size;
  JERRY_CONTEXT(jmem_system_allocator_metadata_size) -=
      SYSTEM_ALLOCATOR_METADATA_SIZE;
  JERRY_CONTEXT(jmem_heap_allocated_blocks_count)--;

  print_total_size_profile_on_alloc(); /* Total size profiling */
  profile_free_end();                  /* Time profiling */
#endif /* !JERRY_SYSTEM_ALLOCATOR */
} /* jmem_heap_free_block_internal */

/**
 * Free the memory block.
 */
void __attr_hot___ jmem_heap_free_block(
    void *ptr,         /**< pointer to beginning of data space of the block */
    const size_t size) /**< size of allocated region */
{
  jmem_heap_free_block_internal(ptr, size, false);
} /* jmem_heap_free_block */

inline void *__attr_hot___ __attr_always_inline___
jmem_heap_alloc_block_small_object(const size_t size) {
  void *ret = jmem_heap_gc_and_alloc_block(size, false, true);
  return ret;
}
inline void __attr_hot___ __attr_always_inline___
jmem_heap_free_block_small_object(void *ptr, const size_t size) {
  jmem_heap_free_block_internal(ptr, size, true);
}

#ifndef JERRY_NDEBUG
/**
 * Check whether the pointer points to the heap
 *
 * Note:
 *      the routine should be used only for assertion checks
 *
 * @return true - if pointer points to the heap,
 *         false - otherwise
 */
bool jmem_is_heap_pointer(const void *pointer) /**< pointer */
{
  bool is_heap_pointer;
#ifndef JERRY_SYSTEM_ALLOCATOR
#ifdef JMEM_SEGMENTED_HEAP
  /* Not yet implemented */
  is_heap_pointer = pointer != NULL;
#else  /* JMEM_SEGMENTED_HEAP */
  is_heap_pointer =
      ((uint8_t *)pointer >= JERRY_HEAP_CONTEXT(area) &&
       (uint8_t *)pointer <= (JERRY_HEAP_CONTEXT(area) + JMEM_HEAP_AREA_SIZE));
#endif /* !JMEM_SEGMENTED_HEAP */
#else  /* JERRY_SYSTEM_ALLOCATOR */
  JERRY_UNUSED(pointer);
  is_heap_pointer = true;
#endif /* !JERRY_SYSTEM_ALLOCATOR */
  return is_heap_pointer;
} /* jmem_is_heap_pointer */
#endif /* !JERRY_NDEBUG */

inline uint32_t __attr_always_inline___
static_compress_pointer_internal(jmem_heap_free_t *p) {
  profile_compression_start();
  profile_compression_cycles_start();
  uint32_t cp = (uint32_t)(p) - (uint32_t)(JERRY_HEAP_CONTEXT(area));
  profile_compression_cycles_end(0);
  profile_compression_end(0); // COMPRESSION_RMC_HIT
  return cp;
}
inline jmem_heap_free_t __attr_always_inline___ *
static_decompress_pointer_internal(uint32_t cp) {
  profile_decompression_start();
  profile_decompression_cycles_start();
  jmem_heap_free_t *p = ((jmem_heap_free_t *)(JERRY_HEAP_CONTEXT(area) + (cp)));
  profile_decompression_cycles_end();
  profile_decompression_end();
  return p;
}

#ifdef JMEM_STATS
/**
 * Get heap memory usage statistics
 */
void jmem_heap_get_stats(
    jmem_heap_stats_t *out_heap_stats_p) /**< [out] heap stats */
{
  JERRY_ASSERT(out_heap_stats_p != NULL);

  *out_heap_stats_p = JERRY_CONTEXT(jmem_heap_stats);
} /* jmem_heap_get_stats */

/**
 * Print heap memory usage statistics
 */
void jmem_heap_stats_print(void) {
  jmem_heap_stats_t *heap_stats = &JERRY_CONTEXT(jmem_heap_stats);

  JERRY_DEBUG_MSG(
      "Heap stats:\n"
      "  Heap size = %zu bytes\n"
      "  Allocated = %zu bytes\n"
      "  Peak allocated = %zu bytes\n"
      "  Waste = %zu bytes\n"
      "  Peak waste = %zu bytes\n"
      "  Allocated byte code data = %zu bytes\n"
      "  Peak allocated byte code data = %zu bytes\n"
      "  Allocated string data = %zu bytes\n"
      "  Peak allocated string data = %zu bytes\n"
      "  Allocated object data = %zu bytes\n"
      "  Peak allocated object data = %zu bytes\n"
      "  Allocated property data = %zu bytes\n"
      "  Peak allocated property data = %zu bytes\n"
      "  Skip-ahead ratio = %zu.%04zu\n"
      "  Average alloc iteration = %zu.%04zu\n"
      "  Average free iteration = %zu.%04zu\n"
      "\n",
      heap_stats->size, heap_stats->allocated_bytes,
      heap_stats->peak_allocated_bytes, heap_stats->waste_bytes,
      heap_stats->peak_waste_bytes, heap_stats->byte_code_bytes,
      heap_stats->peak_byte_code_bytes, heap_stats->string_bytes,
      heap_stats->peak_string_bytes, heap_stats->object_bytes,
      heap_stats->peak_object_bytes, heap_stats->property_bytes,
      heap_stats->peak_property_bytes,
      heap_stats->skip_count / heap_stats->nonskip_count,
      heap_stats->skip_count % heap_stats->nonskip_count * 10000 /
          heap_stats->nonskip_count,
      heap_stats->alloc_iter_count / heap_stats->alloc_count,
      heap_stats->alloc_iter_count % heap_stats->alloc_count * 10000 /
          heap_stats->alloc_count,
      heap_stats->free_iter_count / heap_stats->free_count,
      heap_stats->free_iter_count % heap_stats->free_count * 10000 /
          heap_stats->free_count);
} /* jmem_heap_stats_print */

/**
 * Initalize heap memory usage statistics account structure
 */
static void jmem_heap_stat_init(void) {
  JERRY_CONTEXT(jmem_heap_stats).size = JMEM_HEAP_AREA_SIZE;
} /* jmem_heap_stat_init */

/**
 * Account allocation
 */
static void jmem_heap_stat_alloc(size_t size) /**< Size of allocated block */
{
  const size_t aligned_size =
      (size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;
  const size_t waste_bytes = aligned_size - size;

  jmem_heap_stats_t *heap_stats = &JERRY_CONTEXT(jmem_heap_stats);

  heap_stats->allocated_bytes += aligned_size;
  heap_stats->waste_bytes += waste_bytes;
  heap_stats->alloc_count++;

  if (heap_stats->allocated_bytes > heap_stats->peak_allocated_bytes) {
    heap_stats->peak_allocated_bytes = heap_stats->allocated_bytes;
  }

  if (heap_stats->waste_bytes > heap_stats->peak_waste_bytes) {
    heap_stats->peak_waste_bytes = heap_stats->waste_bytes;
  }
} /* jmem_heap_stat_alloc */

/**
 * Account freeing
 */
static void jmem_heap_stat_free(size_t size) /**< Size of freed block */
{
  const size_t aligned_size =
      (size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;
  const size_t waste_bytes = aligned_size - size;

  jmem_heap_stats_t *heap_stats = &JERRY_CONTEXT(jmem_heap_stats);

  heap_stats->free_count++;
  heap_stats->allocated_bytes -= aligned_size;
  heap_stats->waste_bytes -= waste_bytes;
} /* jmem_heap_stat_free */

/**
 * Counts number of skip-aheads during insertion of free block
 */
static void jmem_heap_stat_skip(void) {
  JERRY_CONTEXT(jmem_heap_stats).skip_count++;
} /* jmem_heap_stat_skip  */

/**
 * Counts number of times we could not skip ahead during free block insertion
 */
static void jmem_heap_stat_nonskip(void) {
  JERRY_CONTEXT(jmem_heap_stats).nonskip_count++;
} /* jmem_heap_stat_nonskip */

/**
 * Count number of iterations required for allocations
 */
static void jmem_heap_stat_alloc_iter(void) {
  JERRY_CONTEXT(jmem_heap_stats).alloc_iter_count++;
} /* jmem_heap_stat_alloc_iter */

/**
 * Counts number of iterations required for inserting free blocks
 */
static void jmem_heap_stat_free_iter(void) {
  JERRY_CONTEXT(jmem_heap_stats).free_iter_count++;
} /* jmem_heap_stat_free_iter */
#endif /* JMEM_STATS */

/**
 * @}
 * @}
 */
