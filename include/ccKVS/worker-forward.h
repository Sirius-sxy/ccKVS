#ifndef WORKER_FORWARD_H
#define WORKER_FORWARD_H

#include "mica.h"
#include "hrd.h"
#include "main.h"  // For struct remote_qp
#include <stdint.h>

/*
 * Server-to-server request forwarding
 *
 * When a server receives a request for a key it doesn't own,
 * it forwards the request to the correct server.
 */

#define FORWARD_UD_QP_ID 3  // QP ID for worker-to-worker forwarding

/*
 * Forwarding request structure
 * Contains original request + client address for direct response
 * Note: Not packed to avoid alignment issues - natural alignment is acceptable
 */
struct forward_req {
	struct mica_op op;           // Original request
	uint32_t client_qpn;         // Original client's QP number
	uint16_t client_lid;         // Original client's LID
	uint8_t original_server;     // Server that received the request
	uint8_t target_server;       // Server that should handle it
	uint64_t request_id;         // For tracking
};

/*
 * Forward a batch of requests to remote servers
 *
 * @param remote_count: Number of requests to forward
 * @param remote_ops: Operations to forward
 * @param remote_indices: Indices in original response array
 * @param remote_machines: Target machine IDs
 * @param cb: Control block with RDMA resources
 * @param wrkr_lid: Local worker ID
 * @param client_info: Array of client connection info for each request
 */
void worker_forward_requests(uint16_t remote_count,
                             struct mica_op **remote_ops,
                             uint16_t *remote_indices,
                             uint8_t *remote_machines,
                             struct hrd_ctrl_blk *cb,
                             uint16_t wrkr_lid,
                             struct remote_qp *client_info);

/*
 * Handle forwarded requests from other servers
 *
 * @param forward_req: The forwarded request
 * @param kv: Local KVS
 * @param resp: Response to fill
 * @param cb: Control block for sending response
 */
void worker_handle_forwarded_request(struct forward_req *forward_req,
                                     struct mica_kv *kv,
                                     struct mica_resp *resp,
                                     struct hrd_ctrl_blk *cb);

/*
 * Check if incoming request is a forwarded request
 *
 * @param buf: Buffer containing the request
 * @param size: Size of the buffer
 * @return: 1 if forwarded request, 0 if normal request
 */
int is_forwarded_request(void *buf, size_t size);

/*
 * Initialize worker-to-worker forwarding
 *
 * @param cb: Control block
 * @param wrkr_lid: Local worker ID
 */
void worker_init_forwarding(struct hrd_ctrl_blk *cb, uint16_t wrkr_lid);

#endif // WORKER_FORWARD_H
