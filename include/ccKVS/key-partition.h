#ifndef KEY_PARTITION_H
#define KEY_PARTITION_H

#include "mica.h"
#include "main.h"
#include <stdint.h>

/*
 * Key partitioning for distributed KVS
 *
 * Each server owns a subset of keys based on hash partitioning.
 * This determines which server should handle a request for a given key.
 */

/*
 * Determine which machine owns this key
 *
 * @param key: The MICA key to check
 * @return: Machine ID (0 to MACHINE_NUM-1) that owns this key
 */
static inline uint8_t get_key_owner_machine(struct mica_key *key) {
    // Use the bucket field for partitioning (already a hash)
    return (uint8_t)(key->bkt % MACHINE_NUM);
}

/*
 * Check if a key belongs to the local machine
 *
 * @param key: The MICA key to check
 * @param local_machine_id: This machine's ID
 * @return: 1 if key is local, 0 if remote
 */
static inline int is_local_key(struct mica_key *key, uint8_t local_machine_id) {
    return get_key_owner_machine(key) == local_machine_id;
}

/*
 * Get the worker ID on a specific machine that should handle this key
 * (Used for load balancing across workers within a machine)
 *
 * @param key: The MICA key
 * @return: Worker ID (0 to WORKERS_PER_MACHINE-1)
 */
static inline uint8_t get_key_owner_worker(struct mica_key *key) {
    return (uint8_t)((key->bkt / MACHINE_NUM) % WORKERS_PER_MACHINE);
}

#endif // KEY_PARTITION_H
