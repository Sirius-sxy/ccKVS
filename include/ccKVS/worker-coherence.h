#ifndef WORKER_COHERENCE_H
#define WORKER_COHERENCE_H

#include "hrd.h"
#include "cache.h"
#include "mica.h"
#include "main.h"

// Coherence protocol constants (adapted from client-side to server-side)
#define WORKER_COH_CREDITS 30                    // Initial credits per server
#define WORKER_COH_CREDITS_IN_MESSAGE 3          // Credits per credit message
#define WORKER_COH_CREDIT_DIVIDER 10
#define WORKER_MAX_BCAST_BATCH 4                 // Max broadcasts per batch
#define WORKER_BCAST_TO_CACHE_BATCH 90           // Max messages polled per batch
#define WORKER_BROADCAST_SS_BATCH 4              // Signal every N broadcasts
#define WORKER_CREDIT_SS_BATCH 1                 // Signal every N credit messages

// Virtual channels for coherence
#define WORKER_UPD_VC 0                          // Update virtual channel
#define WORKER_INV_VC 1                          // Invalidation virtual channel (for LIN)

// QP indices for coherence communication
#define WORKER_BROADCAST_QP_ID 2                 // QP for coherence broadcasts
#define WORKER_FC_QP_ID 3                        // QP for flow control credits

// Buffer configuration for server-side coherence
#define WORKER_COH_BUF_SLOTS ((MACHINE_NUM - 1) * WORKER_COH_CREDITS)
#define WORKER_UD_REQ_SIZE (sizeof(struct ibv_grh) + sizeof(struct mica_op))
#define WORKER_COH_BUF_SIZE (WORKER_UD_REQ_SIZE * WORKER_COH_BUF_SLOTS)

/**
 * Worker coherence context - manages cache coherence state per worker
 */
struct worker_coherence_ctx {
    // Credit tracking - one array per remote server
    uint8_t credits[MACHINE_NUM];              // Available credits per server

    // Broadcast state
    uint16_t br_tx;                             // Broadcast transmit counter
    uint16_t coh_buf_i;                         // Current coherence buffer index

    // Reception state
    int pull_ptr;                               // Circular buffer read pointer
    uint8_t broadcasts_seen[MACHINE_NUM];       // Received broadcasts per server
    uint16_t credit_tx;                         // Credit transmit counter

    // Work requests and scatter-gather lists
    struct ibv_send_wr *coh_send_wr;           // Coherence send work requests
    struct ibv_sge *coh_send_sgl;              // Coherence send SGL
    struct ibv_send_wr *credit_send_wr;        // Credit send work requests
    struct ibv_sge *credit_send_sgl;           // Credit send SGL
    struct ibv_recv_wr *credit_recv_wr;        // Credit receive work requests

    // Buffers
    struct mica_op *coh_buf;                    // Coherence message buffer
    struct ud_req *incoming_reqs;               // Received coherence messages

    // Statistics
    long long num_broadcasts_sent;
    long long num_broadcasts_received;
    long long num_credits_sent;
    long long num_credits_received;
    long long num_stalls_due_to_credits;
};

/**
 * Initialize worker coherence context
 * @param ctx: Coherence context to initialize
 * @param cb: RDMA control block
 * @param wrkr_gid: Global worker ID
 * @return: 0 on success, -1 on failure
 */
int worker_coherence_init(struct worker_coherence_ctx *ctx,
                          struct hrd_ctrl_blk *cb,
                          uint16_t wrkr_gid);

/**
 * Cleanup worker coherence context
 * @param ctx: Coherence context to cleanup
 */
void worker_coherence_cleanup(struct worker_coherence_ctx *ctx);

/**
 * Broadcast invalidations to all other servers after a write operation
 * Adapted from perform_broadcasts_SC() in original ccKVS
 *
 * @param ctx: Worker coherence context
 * @param ops: Array of cache operations (writes marked with CACHE_OP_BRC)
 * @param op_num: Number of operations in array
 * @param cb: RDMA control block
 * @param wrkr_gid: Global worker ID
 * @return: Number of broadcasts sent
 */
uint16_t worker_broadcast_updates(struct worker_coherence_ctx *ctx,
                                   struct cache_op *ops,
                                   uint16_t op_num,
                                   struct hrd_ctrl_blk *cb,
                                   uint16_t wrkr_gid);

/**
 * Poll and receive coherence messages from other servers
 * Adapted from poll_coherence_SC() in original ccKVS
 *
 * @param ctx: Worker coherence context
 * @param update_ops: Buffer to store received updates
 * @param update_resp: Response buffer for updates
 * @param wrkr_gid: Global worker ID
 * @return: Number of coherence messages received
 */
uint16_t worker_poll_coherence(struct worker_coherence_ctx *ctx,
                                struct cache_op *update_ops,
                                struct mica_resp *update_resp,
                                uint16_t wrkr_gid);

/**
 * Apply received coherence updates to local cache
 * @param update_ops: Coherence operations to apply
 * @param update_resp: Response buffer
 * @param update_num: Number of updates
 * @param wrkr_lid: Local worker ID
 * @return: 0 on success
 */
int worker_apply_coherence_updates(struct cache_op *update_ops,
                                    struct mica_resp *update_resp,
                                    uint16_t update_num,
                                    uint16_t wrkr_lid);

/**
 * Check if broadcast credits are available for all servers
 * Internal helper function
 */
static inline bool worker_check_broadcast_credits(struct worker_coherence_ctx *ctx,
                                                   struct hrd_ctrl_blk *cb);

/**
 * Poll for incoming credit messages
 * Internal helper function
 */
static inline void worker_poll_credits(struct worker_coherence_ctx *ctx,
                                       struct hrd_ctrl_blk *cb);

/**
 * Create and send credit messages to sender servers
 * Adapted from create_credits_SC() in original ccKVS
 *
 * @param ctx: Worker coherence context
 * @param coh_wc: Work completions from received coherence messages
 * @param coh_num: Number of coherence messages received
 * @param cb: RDMA control block
 * @param wrkr_gid: Global worker ID
 * @return: Number of credit messages sent
 */
uint16_t worker_create_credits(struct worker_coherence_ctx *ctx,
                                struct ibv_wc *coh_wc,
                                uint16_t coh_num,
                                struct hrd_ctrl_blk *cb,
                                uint16_t wrkr_gid);

#endif // WORKER_COHERENCE_H
