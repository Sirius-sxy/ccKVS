#include "worker-forward.h"
#include "util.h"
#include "key-partition.h"
#include <string.h>

extern struct remote_qp remote_wrkr_qp[WORKER_NUM_UD_QPS][WORKER_NUM];
// Note: machine_id is declared in hrd.h (included via util.h)

/*
 * Forward requests to remote servers using existing worker-to-worker QPs
 *
 * Workers are already connected via remote_wrkr_qp, so we can send directly.
 * The forwarded request includes the original operation plus client info.
 */
void worker_forward_requests(uint16_t remote_count,
                             struct mica_op **remote_ops,
                             uint16_t *remote_indices,
                             uint8_t *remote_machines,
                             struct hrd_ctrl_blk *cb,
                             uint16_t wrkr_lid,
                             struct remote_qp *client_info)
{
	struct ibv_send_wr forward_wr[WORKER_MAX_BATCH];
	struct ibv_sge forward_sgl[WORKER_MAX_BATCH];
	struct ibv_send_wr *bad_wr = NULL;

	// Allocate buffer for forward requests
	struct forward_req *forward_reqs = malloc(remote_count * sizeof(struct forward_req));

	for(uint16_t i = 0; i < remote_count; i++) {
		uint8_t target_machine = remote_machines[i];
		struct mica_op *op = remote_ops[i];

		// Calculate target worker on remote machine
		uint8_t target_worker = get_key_owner_worker(&op->key);
		uint16_t target_wrkr_gid = target_machine * WORKERS_PER_MACHINE + target_worker;

		// Prepare forward request
		memcpy(&forward_reqs[i].op, op, sizeof(struct mica_op));
		forward_reqs[i].client_qpn = 0;  // TODO: Get from original request
		forward_reqs[i].client_lid = 0;  // TODO: Get from original request
		forward_reqs[i].original_server = machine_id;
		forward_reqs[i].target_server = target_machine;
		forward_reqs[i].request_id = remote_indices[i];

		// Setup SGE for this request
		forward_sgl[i].addr = (uint64_t)&forward_reqs[i];
		forward_sgl[i].length = sizeof(struct forward_req);
		forward_sgl[i].lkey = cb->dgram_buf_mr->lkey;

		// Setup WR for forwarding
		memset(&forward_wr[i], 0, sizeof(struct ibv_send_wr));
		forward_wr[i].wr_id = i;
		forward_wr[i].opcode = IBV_WR_SEND;
		forward_wr[i].sg_list = &forward_sgl[i];
		forward_wr[i].num_sge = 1;
		forward_wr[i].send_flags = IBV_SEND_SIGNALED;

		// Use existing worker-to-worker connection
		forward_wr[i].wr.ud.ah = remote_wrkr_qp[0][target_wrkr_gid].ah;
		forward_wr[i].wr.ud.remote_qpn = remote_wrkr_qp[0][target_wrkr_gid].qpn;
		forward_wr[i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

		// Link to next WR
		if(i < remote_count - 1) {
			forward_wr[i].next = &forward_wr[i + 1];
		} else {
			forward_wr[i].next = NULL;
		}
	}

	// Post all forwarding requests at once
	if(remote_count > 0) {
		int ret = ibv_post_send(cb->dgram_qp[0], &forward_wr[0], &bad_wr);
		if(ret != 0) {
			fprintf(stderr, "Worker %d: Failed to forward %d requests: %s\n",
			        wrkr_lid, remote_count, strerror(ret));
		}
	}

	// Note: We don't wait for completion here - fire and forget
	// The target worker will handle the request and respond to client
	// We'll need to track these for proper cleanup

	free(forward_reqs);
}

/*
 * Handle a forwarded request from another worker
 *
 * Process the request locally (since this server owns the key)
 * and send response back to the original client.
 */
void worker_handle_forwarded_request(struct forward_req *forward_req,
                                     struct mica_kv *kv,
                                     struct mica_resp *resp,
                                     struct hrd_ctrl_blk *cb)
{
	// Access the operation directly without taking address to avoid alignment warning
	struct mica_op *op_ptr = &forward_req->op;

	// Query local KVS (we own this key)
	KVS_BATCH_OP(kv, 1, &op_ptr, resp);

	// TODO: Send response back to original client
	// Would need to use forward_req->client_qpn and client_lid
	// to construct the address handle and send the response

	// For now, we've processed the request but response handling
	// needs to be integrated with the main worker loop
}

/*
 * Initialize worker-to-worker forwarding
 *
 * Workers are already connected via remote_wrkr_qp (created in util.c),
 * so this is just a placeholder for any additional setup.
 */
void worker_init_forwarding(struct hrd_ctrl_blk *cb, uint16_t wrkr_lid)
{
	// Workers are already connected in create_AHs_for_worker()
	// No additional initialization needed for basic forwarding

	(void)cb;
	(void)wrkr_lid;
}

/*
 * Check if incoming request is a forwarded request
 *
 * Forwarded requests have a different size (struct forward_req)
 * compared to normal requests (struct mica_op).
 */
int is_forwarded_request(void *buf, size_t size)
{
	return (size == sizeof(struct forward_req));
}
