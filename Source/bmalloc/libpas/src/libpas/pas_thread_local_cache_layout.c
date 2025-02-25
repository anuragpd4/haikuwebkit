/*
 * Copyright (c) 2018-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "pas_config.h"

#if LIBPAS_ENABLED

#include "pas_thread_local_cache_layout.h"

#include "pas_heap_lock.h"
#include "pas_large_utility_free_heap.h"
#include "pas_local_view_cache_node.h"
#include "pas_redundant_local_allocator_node.h"
#include "pas_segregated_size_directory_inlines.h"
#include "pas_utils.h"

pas_thread_local_cache_layout_node pas_thread_local_cache_layout_first_node = NULL;
pas_thread_local_cache_layout_node pas_thread_local_cache_layout_last_node = NULL;
pas_allocator_index pas_thread_local_cache_layout_next_allocator_index =
    PAS_LOCAL_ALLOCATOR_UNSELECTED_NUM_INDICES;
pas_thread_local_cache_layout_hashtable pas_thread_local_cache_layout_hashtable_instance =
    PAS_HASHTABLE_INITIALIZER;

PAS_DEFINE_LOCK(pas_thread_local_cache_layout_hashtable);

pas_allocator_index pas_thread_local_cache_layout_add_node(pas_thread_local_cache_layout_node node)
{
    static const bool verbose = false;

    pas_allocator_index result;
    bool did_overflow;
    pas_compact_thread_local_cache_layout_node compact_node;

    pas_heap_lock_assert_held();

    switch (pas_thread_local_cache_layout_node_get_kind(node)) {
    case pas_thread_local_cache_layout_segregated_size_directory_node_kind:
    case pas_thread_local_cache_layout_redundant_local_allocator_node_kind:
        PAS_ASSERT(!pas_thread_local_cache_layout_node_get_allocator_index_generic(node));
        break;
    case pas_thread_local_cache_layout_local_view_cache_node_kind:
        PAS_ASSERT(
            pas_thread_local_cache_layout_node_get_allocator_index_generic(node)
            == (pas_allocator_index)UINT_MAX);
        break;
    }

    PAS_ASSERT(!pas_thread_local_cache_layout_node_get_next(node));
    
    result = pas_thread_local_cache_layout_next_allocator_index;

    PAS_ASSERT(result < (pas_allocator_index)UINT_MAX);
    pas_thread_local_cache_layout_node_set_allocator_index(node, result);

    if (verbose) {
        pas_log("pas_thread_local_cache_layout_add_node: node = %p, allocator_index = %u, "
                "num_allocator_indices = %u\n",
                node, result, pas_thread_local_cache_layout_node_num_allocator_indices(node));
    }

    did_overflow = __builtin_add_overflow(
        pas_thread_local_cache_layout_next_allocator_index,
        pas_thread_local_cache_layout_node_num_allocator_indices(node),
        &pas_thread_local_cache_layout_next_allocator_index);
    PAS_ASSERT(!did_overflow);

    pas_fence();
    
    if (!pas_thread_local_cache_layout_first_node) {
        PAS_ASSERT(!pas_thread_local_cache_layout_last_node);
        PAS_ASSERT(result == PAS_LOCAL_ALLOCATOR_UNSELECTED_NUM_INDICES);
        pas_thread_local_cache_layout_first_node = node;
        pas_thread_local_cache_layout_last_node = node;
    } else {
        PAS_ASSERT(pas_thread_local_cache_layout_last_node);
        PAS_ASSERT(result > PAS_LOCAL_ALLOCATOR_UNSELECTED_NUM_INDICES);
        pas_thread_local_cache_layout_node_set_next(pas_thread_local_cache_layout_last_node, node);
        pas_thread_local_cache_layout_last_node = node;
    }

    pas_thread_local_cache_layout_hashtable_lock_lock();
    pas_compact_thread_local_cache_layout_node_store(&compact_node, node);
    pas_thread_local_cache_layout_hashtable_add_new(
        &pas_thread_local_cache_layout_hashtable_instance, compact_node,
        NULL, &pas_large_utility_free_heap_allocation_config);
    pas_thread_local_cache_layout_hashtable_lock_unlock();
    
    return result;
}

pas_allocator_index pas_thread_local_cache_layout_add(
    pas_segregated_size_directory* directory)
{
    static const bool verbose = false;

    if (verbose) {
        pas_log("Adding TLC layout node for directory = %p, object_size = %u, page_config_kind = %s\n",
                directory, directory->object_size,
                pas_segregated_page_config_kind_get_string(directory->base.page_config_kind));
    }
    
    return pas_thread_local_cache_layout_add_node(
        pas_wrap_segregated_size_directory(directory));
}

pas_allocator_index pas_thread_local_cache_layout_duplicate(
    pas_segregated_size_directory* directory)
{
    pas_redundant_local_allocator_node* redundant_node;

    redundant_node = pas_redundant_local_allocator_node_create(directory);

    return pas_thread_local_cache_layout_add_node(
        pas_wrap_redundant_local_allocator_node(redundant_node));
}

pas_allocator_index pas_thread_local_cache_layout_add_view_cache(
    pas_segregated_size_directory* directory)
{
    pas_local_view_cache_node* cache_node;

    cache_node = pas_local_view_cache_node_create(directory);

    return pas_thread_local_cache_layout_add_node(pas_wrap_local_view_cache_node(cache_node));
}

pas_thread_local_cache_layout_node pas_thread_local_cache_layout_get_node_for_index(pas_allocator_index index)
{
    pas_compact_thread_local_cache_layout_node compact_node;

    pas_thread_local_cache_layout_hashtable_lock_lock();
    compact_node = pas_thread_local_cache_layout_hashtable_get(
        &pas_thread_local_cache_layout_hashtable_instance, index);
    pas_thread_local_cache_layout_hashtable_lock_unlock();

    return pas_compact_thread_local_cache_layout_node_load(&compact_node);
}

#endif /* LIBPAS_ENABLED */
