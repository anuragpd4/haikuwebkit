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

#ifndef PAS_THREAD_LOCAL_CACHE_LAYOUT_H
#define PAS_THREAD_LOCAL_CACHE_LAYOUT_H

#include "pas_allocator_index.h"
#include "pas_config.h"
#include "pas_hashtable.h"
#include "pas_lock.h"
#include "pas_thread_local_cache_layout_entry.h"
#include "pas_thread_local_cache_layout_node.h"
#include "pas_utils.h"

PAS_BEGIN_EXTERN_C;

PAS_API extern pas_thread_local_cache_layout_node pas_thread_local_cache_layout_first_node;
PAS_API extern pas_thread_local_cache_layout_node pas_thread_local_cache_layout_last_node;

PAS_CREATE_HASHTABLE(pas_thread_local_cache_layout_hashtable,
                     pas_thread_local_cache_layout_entry,
                     pas_thread_local_cache_layout_key);

/* Lock used internally for accessing the hashtable_instance. You don't have to use this lock unless you
   access the hashtable_instance directly. */
PAS_DECLARE_LOCK(pas_thread_local_cache_layout_hashtable);

PAS_API extern pas_thread_local_cache_layout_hashtable pas_thread_local_cache_layout_hashtable_instance;

/* Clients can use this to force the next call to add to go to this index. */
PAS_API extern pas_allocator_index pas_thread_local_cache_layout_next_allocator_index;

PAS_API pas_allocator_index pas_thread_local_cache_layout_add_node(
    pas_thread_local_cache_layout_node node);

PAS_API pas_allocator_index pas_thread_local_cache_layout_add(
    pas_segregated_size_directory* directory);

PAS_API pas_allocator_index pas_thread_local_cache_layout_duplicate(
    pas_segregated_size_directory* directory);

PAS_API pas_allocator_index pas_thread_local_cache_layout_add_view_cache(
    pas_segregated_size_directory* directory);

/* You don't need to hold any locks to use this because this uses its own lock behind the scenes. */
PAS_API pas_thread_local_cache_layout_node pas_thread_local_cache_layout_get_node_for_index(
    pas_allocator_index index);

#define PAS_THREAD_LOCAL_CACHE_LAYOUT_EACH_ALLOCATOR(node) \
    node = pas_thread_local_cache_layout_first_node; \
    node; \
    node = pas_thread_local_cache_layout_node_get_next(node)
        

PAS_END_EXTERN_C;

#endif /* PAS_THREAD_LOCAL_CACHE_LAYOUT_H */

