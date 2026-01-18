#include "util.h"
#include "cache.h"

/*
 * Server-side cache lookup for worker threads
 *
 * This function checks the server-side cache before accessing the KVS.
 * It separates cache hits from misses and only queries KVS for misses.
 */

extern struct cache cache;  // Global cache shared by all workers

/*
 * Query server-side cache for a batch of requests
 *
 * @param wr_i: Number of requests in the batch
 * @param op_ptr_arr: Array of pointers to MICA operations
 * @param resp_arr: Response array to fill
 * @param cache_miss_ops: Output array for cache misses (need KVS lookup)
 * @param cache_miss_indices: Indices of cache misses in original array
 * @return: Number of cache misses (need to query KVS)
 */
uint16_t worker_query_cache(uint16_t wr_i,
                            struct mica_op **op_ptr_arr,
                            struct mica_resp *resp_arr,
                            struct mica_op **cache_miss_ops,
                            uint16_t *cache_miss_indices)
{
	uint16_t I, j;
	uint16_t cache_miss_count = 0;

	// Batch variables for cache lookup
	unsigned int bkt[WORKER_MAX_BATCH];
	struct mica_bkt *bkt_ptr[WORKER_MAX_BATCH];
	unsigned int tag[WORKER_MAX_BATCH];
	int key_in_cache[WORKER_MAX_BATCH];
	struct cache_op *cache_ptr[WORKER_MAX_BATCH];

	// Phase 1: Compute bucket and tag for all requests
	for(I = 0; I < wr_i; I++) {
		bkt[I] = op_ptr_arr[I]->key.bkt & cache.hash_table.bkt_mask;
		bkt_ptr[I] = &cache.hash_table.ht_index[bkt[I]];
		__builtin_prefetch(bkt_ptr[I], 0, 0);
		tag[I] = op_ptr_arr[I]->key.tag;

		key_in_cache[I] = 0;
		cache_ptr[I] = NULL;
	}

	// Phase 2: Lookup in cache hash table
	for(I = 0; I < wr_i; I++) {
		for(j = 0; j < 8; j++) {
			if(bkt_ptr[I]->slots[j].in_use == 1 &&
			   bkt_ptr[I]->slots[j].tag == tag[I]) {
				uint64_t log_offset = bkt_ptr[I]->slots[j].offset &
									  cache.hash_table.log_mask;

				cache_ptr[I] = (struct cache_op *) &cache.hash_table.ht_log[log_offset];

				__builtin_prefetch(cache_ptr[I], 0, 0);
				__builtin_prefetch((uint8_t *) cache_ptr[I] + 64, 0, 0);

				// Check if entry is still valid
				if(cache.hash_table.log_head - bkt_ptr[I]->slots[j].offset >=
				   cache.hash_table.log_cap) {
					cache_ptr[I] = NULL;
				}

				break;
			}
		}
	}

	// Phase 3: Verify key match and read values
	cache_meta prev_meta;
	for(I = 0; I < wr_i; I++) {
		if(cache_ptr[I] != NULL) {
			// Compare keys
			long long *key_ptr_cache = (long long *) cache_ptr[I];
			long long *key_ptr_req = (long long *) op_ptr_arr[I];

			if(key_ptr_cache[1] == key_ptr_req[1]) {
				// Cache hit!
				key_in_cache[I] = 1;

				if(op_ptr_arr[I]->opcode == MICA_OP_GET) {
					// Lock-free read with versioning
					do {
						prev_meta = cache_ptr[I]->key.meta;
						resp_arr[I].val_ptr = cache_ptr[I]->value;
						resp_arr[I].val_len = cache_ptr[I]->val_len;
					} while (!optik_is_same_version_and_valid(prev_meta,
					                                           cache_ptr[I]->key.meta));
					resp_arr[I].type = MICA_RESP_GET_SUCCESS;
				}
				else if(op_ptr_arr[I]->opcode == MICA_OP_PUT) {
					// Write to cache (will also need to write to KVS)
					// For now, treat writes as cache misses
					key_in_cache[I] = 0;
				}
			}
		}

		// Collect cache misses
		if(key_in_cache[I] == 0) {
			cache_miss_ops[cache_miss_count] = op_ptr_arr[I];
			cache_miss_indices[cache_miss_count] = I;
			cache_miss_count++;
		}
	}

	return cache_miss_count;
}

/*
 * Merge cache responses with KVS responses
 *
 * @param wr_i: Total number of requests
 * @param resp_arr: Final response array (already has cache hits)
 * @param kvs_resp: Responses from KVS
 * @param cache_miss_indices: Indices where KVS responses should go
 * @param cache_miss_count: Number of KVS responses
 */
void worker_merge_responses(uint16_t wr_i,
                           struct mica_resp *resp_arr,
                           struct mica_resp *kvs_resp,
                           uint16_t *cache_miss_indices,
                           uint16_t cache_miss_count)
{
	for(uint16_t i = 0; i < cache_miss_count; i++) {
		uint16_t idx = cache_miss_indices[i];
		resp_arr[idx] = kvs_resp[i];
	}
}
