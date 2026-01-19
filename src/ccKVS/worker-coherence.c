#include "worker-coherence.h"
#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// External references
// Note: machine_id is declared in hrd.h (included via worker-coherence.h)
extern struct remote_qp **remote_wrkr_qp;

/**
 * Initialize worker coherence context
 * Allocates buffers and sets up work requests for coherence protocol
 */
int worker_coherence_init(struct worker_coherence_ctx *ctx,
                          struct hrd_ctrl_blk *cb,
                          uint16_t wrkr_gid)
{
    memset(ctx, 0, sizeof(struct worker_coherence_ctx));

    // Initialize credits - start with full credits for each remote server
    for (uint16_t i = 0; i < MACHINE_NUM; i++) {
        if (i == machine_id) {
            ctx->credits[i] = 0;  // No credits for self
        } else {
            ctx->credits[i] = WORKER_COH_CREDITS;  // 30 credits per server
        }
    }

    // Allocate coherence message buffer (for sending)
    ctx->coh_buf = (struct mica_op*)calloc(WORKER_MAX_BCAST_BATCH * (MACHINE_NUM - 1),
                                           sizeof(struct mica_op));
    if (!ctx->coh_buf) {
        fprintf(stderr, "Worker %d: Failed to allocate coherence buffer\n", wrkr_gid);
        return -1;
    }

    // Allocate incoming request buffer (circular buffer for receiving)
    ctx->incoming_reqs = (struct ud_req*)calloc(WORKER_COH_BUF_SLOTS,
                                                sizeof(struct ud_req));
    if (!ctx->incoming_reqs) {
        fprintf(stderr, "Worker %d: Failed to allocate incoming requests buffer\n", wrkr_gid);
        free(ctx->coh_buf);
        return -1;
    }

    // Allocate work requests for coherence broadcasts
    int num_coh_wrs = WORKER_MAX_BCAST_BATCH * (MACHINE_NUM - 1);
    ctx->coh_send_wr = (struct ibv_send_wr*)calloc(num_coh_wrs, sizeof(struct ibv_send_wr));
    ctx->coh_send_sgl = (struct ibv_sge*)calloc(num_coh_wrs, sizeof(struct ibv_sge));

    if (!ctx->coh_send_wr || !ctx->coh_send_sgl) {
        fprintf(stderr, "Worker %d: Failed to allocate coherence WRs\n", wrkr_gid);
        worker_coherence_cleanup(ctx);
        return -1;
    }

    // Allocate work requests for credit messages
    ctx->credit_send_wr = (struct ibv_send_wr*)calloc(MACHINE_NUM, sizeof(struct ibv_send_wr));
    ctx->credit_send_sgl = (struct ibv_sge*)calloc(MACHINE_NUM, sizeof(struct ibv_sge));
    ctx->credit_recv_wr = (struct ibv_recv_wr*)calloc(MACHINE_NUM, sizeof(struct ibv_recv_wr));

    if (!ctx->credit_send_wr || !ctx->credit_send_sgl || !ctx->credit_recv_wr) {
        fprintf(stderr, "Worker %d: Failed to allocate credit WRs\n", wrkr_gid);
        worker_coherence_cleanup(ctx);
        return -1;
    }

    // Setup coherence send work requests
    for (int j = 0; j < WORKER_MAX_BCAST_BATCH; j++) {
        for (int i = 0; i < MACHINE_NUM - 1; i++) {
            int index = j * (MACHINE_NUM - 1) + i;

            // Calculate remote machine ID (skip local machine)
            uint16_t rm_id = (i < machine_id) ? i : (i + 1);

            // Calculate target worker global ID on remote machine
            uint16_t local_wrkr_id = wrkr_gid % WORKERS_PER_MACHINE;
            uint16_t target_wrkr_gid = rm_id * WORKERS_PER_MACHINE + local_wrkr_id;

            // Setup scatter-gather list
            ctx->coh_send_sgl[index].addr = (uint64_t)&ctx->coh_buf[index];
            ctx->coh_send_sgl[index].length = sizeof(struct mica_op);
            ctx->coh_send_sgl[index].lkey = cb->dgram_buf_mr->lkey;

            // Setup send work request
            ctx->coh_send_wr[index].wr_id = index;
            ctx->coh_send_wr[index].sg_list = &ctx->coh_send_sgl[index];
            ctx->coh_send_wr[index].num_sge = 1;
            ctx->coh_send_wr[index].opcode = IBV_WR_SEND_WITH_IMM;
            ctx->coh_send_wr[index].imm_data = (uint32_t)machine_id;  // Sender machine ID

            // Use existing worker-to-worker connections
            if (remote_wrkr_qp && remote_wrkr_qp[0]) {
                ctx->coh_send_wr[index].wr.ud.ah = remote_wrkr_qp[0][target_wrkr_gid].ah;
                ctx->coh_send_wr[index].wr.ud.remote_qpn = remote_wrkr_qp[0][target_wrkr_gid].qpn;
                ctx->coh_send_wr[index].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;
            }

            // Signal every WORKER_BROADCAST_SS_BATCH sends
            if ((index + 1) % WORKER_BROADCAST_SS_BATCH == 0) {
                ctx->coh_send_wr[index].send_flags = IBV_SEND_SIGNALED;
            } else {
                ctx->coh_send_wr[index].send_flags = IBV_SEND_INLINE;
            }

            // Link work requests together
            if (index < num_coh_wrs - 1) {
                ctx->coh_send_wr[index].next = &ctx->coh_send_wr[index + 1];
            } else {
                ctx->coh_send_wr[index].next = NULL;
            }
        }
    }

    printf("Worker %d: Coherence context initialized (credits=%d per server)\n",
           wrkr_gid, WORKER_COH_CREDITS);

    return 0;
}

/**
 * Cleanup worker coherence context
 */
void worker_coherence_cleanup(struct worker_coherence_ctx *ctx)
{
    if (ctx->coh_buf) free(ctx->coh_buf);
    if (ctx->incoming_reqs) free(ctx->incoming_reqs);
    if (ctx->coh_send_wr) free(ctx->coh_send_wr);
    if (ctx->coh_send_sgl) free(ctx->coh_send_sgl);
    if (ctx->credit_send_wr) free(ctx->credit_send_wr);
    if (ctx->credit_send_sgl) free(ctx->credit_send_sgl);
    if (ctx->credit_recv_wr) free(ctx->credit_recv_wr);
    memset(ctx, 0, sizeof(struct worker_coherence_ctx));
}

/**
 * Check if broadcast credits are available for all servers
 */
static inline bool worker_check_broadcast_credits(struct worker_coherence_ctx *ctx,
                                                   struct hrd_ctrl_blk *cb)
{
    bool poll_for_credits = false;

    // Check if any machine has zero credits
    for (uint16_t j = 0; j < MACHINE_NUM; j++) {
        if (j == machine_id) continue;
        if (ctx->credits[j] == 0) {
            poll_for_credits = true;
            break;
        }
    }

    // If any machine is out of credits, poll for incoming credits
    if (poll_for_credits) {
        worker_poll_credits(ctx, cb);
    }

    // Verify all machines have credits
    for (uint16_t j = 0; j < MACHINE_NUM; j++) {
        if (j == machine_id) continue;
        if (ctx->credits[j] == 0) {
            ctx->num_stalls_due_to_credits++;
            return false;  // Cannot broadcast
        }
    }

    return true;
}

/**
 * Poll for incoming credit messages
 */
static inline void worker_poll_credits(struct worker_coherence_ctx *ctx,
                                       struct hrd_ctrl_blk *cb)
{
    struct ibv_wc credit_wc[MACHINE_NUM];
    int num_comps = ibv_poll_cq(cb->dgram_recv_cq[WORKER_FC_QP_ID], MACHINE_NUM, credit_wc);

    for (int i = 0; i < num_comps; i++) {
        if (credit_wc[i].status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Credit WC failed: status=%d\n", credit_wc[i].status);
            continue;
        }

        // Extract sender machine ID from immediate data
        uint16_t sender_machine = credit_wc[i].imm_data;
        if (sender_machine < MACHINE_NUM && sender_machine != machine_id) {
            // Each credit message restores WORKER_COH_CREDITS_IN_MESSAGE credits
            ctx->credits[sender_machine] += WORKER_COH_CREDITS_IN_MESSAGE;
            ctx->num_credits_received++;

            // Cap at maximum credits
            if (ctx->credits[sender_machine] > WORKER_COH_CREDITS) {
                ctx->credits[sender_machine] = WORKER_COH_CREDITS;
            }
        }
    }
}

/**
 * Broadcast updates to all other servers
 * Adapted from perform_broadcasts_SC() in original ccKVS
 */
uint16_t worker_broadcast_updates(struct worker_coherence_ctx *ctx,
                                   struct cache_op *ops,
                                   uint16_t op_num,
                                   struct hrd_ctrl_blk *cb,
                                   uint16_t wrkr_gid)
{
    uint16_t op_i = 0;
    uint16_t br_i = 0;  // Broadcast batch index
    uint16_t total_broadcasts = 0;

    // Scan operations for broadcasts (CACHE_OP_BRC indicates need to broadcast)
    while (op_i < op_num) {
        if (ops[op_i].opcode != CACHE_OP_BRC) {
            op_i++;
            continue;
        }

        // Check if credits are available for all remote machines
        if (!worker_check_broadcast_credits(ctx, cb)) {
            break;  // Stall - cannot broadcast without credits
        }

        // Prepare broadcast message for all remote servers
        for (uint16_t i = 0; i < MACHINE_NUM - 1; i++) {
            int buf_index = br_i * (MACHINE_NUM - 1) + i;

            // Copy operation to coherence buffer, change opcode to UPDATE
            memcpy(&ctx->coh_buf[buf_index], &ops[op_i], sizeof(struct mica_op));
            ctx->coh_buf[buf_index].opcode = CACHE_OP_UPD;  // Send as UPDATE
        }

        // Decrement credits from all remote machines
        for (uint16_t j = 0; j < MACHINE_NUM; j++) {
            if (j != machine_id) {
                ctx->credits[j]--;
            }
        }

        br_i++;
        op_i++;

        // Post batch when full
        if (br_i == WORKER_MAX_BCAST_BATCH) {
            struct ibv_send_wr *bad_wr;
            int ret = ibv_post_send(cb->dgram_qp[0], ctx->coh_send_wr, &bad_wr);
            if (ret != 0) {
                fprintf(stderr, "Worker %d: Failed to post broadcast sends: %d\n", wrkr_gid, ret);
            }
            total_broadcasts += br_i;
            ctx->num_broadcasts_sent += br_i * (MACHINE_NUM - 1);
            br_i = 0;
        }
    }

    // Post remaining broadcasts
    if (br_i > 0) {
        // Only post the work requests we actually need
        int num_wrs = br_i * (MACHINE_NUM - 1);
        ctx->coh_send_wr[num_wrs - 1].next = NULL;  // Terminate chain

        struct ibv_send_wr *bad_wr;
        int ret = ibv_post_send(cb->dgram_qp[0], ctx->coh_send_wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "Worker %d: Failed to post final broadcast sends: %d\n", wrkr_gid, ret);
        }

        // Restore chain for next time
        ctx->coh_send_wr[num_wrs - 1].next = &ctx->coh_send_wr[num_wrs];

        total_broadcasts += br_i;
        ctx->num_broadcasts_sent += br_i * (MACHINE_NUM - 1);
    }

    return total_broadcasts;
}

/**
 * Poll and receive coherence messages from other servers
 * Adapted from poll_coherence_SC() in original ccKVS
 */
uint16_t worker_poll_coherence(struct worker_coherence_ctx *ctx,
                                struct cache_op *update_ops,
                                struct mica_resp *update_resp,
                                uint16_t wrkr_gid)
{
    uint16_t coh_i = 0;

    // Poll up to WORKER_BCAST_TO_CACHE_BATCH messages
    while (coh_i < WORKER_BCAST_TO_CACHE_BATCH) {
        // Calculate next slot in circular buffer
        int next_slot = (ctx->pull_ptr + 1) % WORKER_COH_BUF_SLOTS;

        // Check if next message is valid (opcode == CACHE_OP_UPD)
        if (ctx->incoming_reqs[next_slot].m_op.opcode != CACHE_OP_UPD) {
            if (ctx->incoming_reqs[next_slot].m_op.opcode != 0) {
                fprintf(stderr, "Worker %d: Invalid coherence opcode: %d\n",
                        wrkr_gid, ctx->incoming_reqs[next_slot].m_op.opcode);
            }
            break;  // No more messages
        }

        // Advance pointer
        ctx->pull_ptr = next_slot;

        // Copy message to update buffer
        memcpy(&update_ops[coh_i], &ctx->incoming_reqs[next_slot].m_op,
               sizeof(struct mica_op));

        // Initialize response
        update_resp[coh_i].type = EMPTY;

        // Mark slot as consumed
        ctx->incoming_reqs[next_slot].m_op.opcode = 0;

        coh_i++;
        ctx->num_broadcasts_received++;
    }

    return coh_i;
}

/**
 * Apply received coherence updates to local cache
 */
int worker_apply_coherence_updates(struct cache_op *update_ops,
                                    struct mica_resp *update_resp,
                                    uint16_t update_num,
                                    uint16_t wrkr_lid)
{
    if (update_num == 0) return 0;

    // Convert cache_op to cache_op* array for batch operation
    struct cache_op *op_ptrs[WORKER_BCAST_TO_CACHE_BATCH];
    for (uint16_t i = 0; i < update_num; i++) {
        op_ptrs[i] = &update_ops[i];
    }

    // Apply updates to cache using existing cache batch operation
    // Use Sequential Consistency cache operation
    cache_batch_op_sc_with_cache_op(update_num, wrkr_lid, op_ptrs, update_resp);

    return 0;
}

/**
 * Create and send credit messages to sender servers
 * Adapted from create_credits_SC() in original ccKVS
 */
uint16_t worker_create_credits(struct worker_coherence_ctx *ctx,
                                struct ibv_wc *coh_wc,
                                uint16_t coh_num,
                                struct hrd_ctrl_blk *cb,
                                uint16_t wrkr_gid)
{
    uint16_t credit_wr_i = 0;

    // For each received coherence message
    for (uint16_t i = 0; i < coh_num; i++) {
        if (coh_wc[i].status != IBV_WC_SUCCESS) continue;

        // Extract sender machine ID from immediate data
        uint16_t rm_id = coh_wc[i].imm_data;
        if (rm_id >= MACHINE_NUM || rm_id == machine_id) continue;

        ctx->broadcasts_seen[rm_id]++;

        // Send credit message after receiving WORKER_COH_CREDITS_IN_MESSAGE updates
        if (ctx->broadcasts_seen[rm_id] == WORKER_COH_CREDITS_IN_MESSAGE) {
            // Calculate target worker on sender machine
            uint16_t local_wrkr_id = wrkr_gid % WORKERS_PER_MACHINE;
            uint16_t target_wrkr_gid = rm_id * WORKERS_PER_MACHINE + local_wrkr_id;

            // Setup credit send work request
            ctx->credit_send_sgl[credit_wr_i].length = 0;  // Zero-byte message
            ctx->credit_send_wr[credit_wr_i].wr_id = credit_wr_i;
            ctx->credit_send_wr[credit_wr_i].sg_list = &ctx->credit_send_sgl[credit_wr_i];
            ctx->credit_send_wr[credit_wr_i].num_sge = 0;
            ctx->credit_send_wr[credit_wr_i].opcode = IBV_WR_SEND_WITH_IMM;
            ctx->credit_send_wr[credit_wr_i].imm_data = (uint32_t)machine_id;

            // Use existing worker-to-worker connections
            if (remote_wrkr_qp && remote_wrkr_qp[0]) {
                ctx->credit_send_wr[credit_wr_i].wr.ud.ah = remote_wrkr_qp[0][target_wrkr_gid].ah;
                ctx->credit_send_wr[credit_wr_i].wr.ud.remote_qpn = remote_wrkr_qp[0][target_wrkr_gid].qpn;
                ctx->credit_send_wr[credit_wr_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;
            }

            // Signal every WORKER_CREDIT_SS_BATCH sends
            if (ctx->credit_tx % WORKER_CREDIT_SS_BATCH == 0) {
                ctx->credit_send_wr[credit_wr_i].send_flags = IBV_SEND_SIGNALED;
            } else {
                ctx->credit_send_wr[credit_wr_i].send_flags = IBV_SEND_INLINE;
            }

            // Link work requests
            if (credit_wr_i > 0) {
                ctx->credit_send_wr[credit_wr_i - 1].next = &ctx->credit_send_wr[credit_wr_i];
            }
            ctx->credit_send_wr[credit_wr_i].next = NULL;

            ctx->broadcasts_seen[rm_id] = 0;
            ctx->credit_tx++;
            ctx->num_credits_sent++;
            credit_wr_i++;
        }
    }

    // Post credit messages
    if (credit_wr_i > 0) {
        struct ibv_send_wr *bad_wr;
        int ret = ibv_post_send(cb->dgram_qp[WORKER_FC_QP_ID],
                               ctx->credit_send_wr, &bad_wr);
        if (ret != 0) {
            fprintf(stderr, "Worker %d: Failed to post credit sends: %d\n", wrkr_gid, ret);
        }
    }

    return credit_wr_i;
}
