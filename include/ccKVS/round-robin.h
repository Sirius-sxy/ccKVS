#ifndef ROUND_ROBIN_H
#define ROUND_ROBIN_H

#include "main.h"
#include <stdint.h>

/*
 * Round-robin scheduler for distributing requests across servers
 *
 * Instead of hash-based key partitioning, use round-robin to evenly
 * distribute requests across all server nodes.
 */

/*
 * Get next server using round-robin
 *
 * @param counter: Pointer to request counter (will be incremented)
 * @return: Server machine ID (0 to MACHINE_NUM-1)
 */
static inline uint8_t get_next_server_rr(uint64_t *counter) {
    uint8_t server = (*counter) % MACHINE_NUM;
    (*counter)++;
    return server;
}

/*
 * Get next worker on a specific server using round-robin
 *
 * @param counter: Pointer to request counter
 * @return: Worker ID (0 to WORKERS_PER_MACHINE-1)
 */
static inline uint8_t get_next_worker_rr(uint64_t *counter) {
    return ((*counter) / MACHINE_NUM) % WORKERS_PER_MACHINE;
}

#endif // ROUND_ROBIN_H
