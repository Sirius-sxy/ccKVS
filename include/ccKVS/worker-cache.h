#ifndef WORKER_CACHE_H
#define WORKER_CACHE_H

#include "mica.h"
#include <stdint.h>

/*
 * Server-side cache functions for worker threads
 */

/*
 * Query server-side cache for a batch of requests
 * Returns number of cache misses that need KVS lookup
 */
uint16_t worker_query_cache(uint16_t wr_i,
                            struct mica_op **op_ptr_arr,
                            struct mica_resp *resp_arr,
                            struct mica_op **cache_miss_ops,
                            uint16_t *cache_miss_indices);

/*
 * Merge cache responses with KVS responses
 */
void worker_merge_responses(uint16_t wr_i,
                           struct mica_resp *resp_arr,
                           struct mica_resp *kvs_resp,
                           uint16_t *cache_miss_indices,
                           uint16_t cache_miss_count);

#endif // WORKER_CACHE_H
