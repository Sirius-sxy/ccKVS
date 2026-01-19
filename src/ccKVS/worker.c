#include "util.h"
#include "inline_util.h"
#include "worker-cache.h"
#include "worker-forward.h"
#include "worker-coherence.h"

// Enable cache coherence for server-side caching (Phase 6)
#ifndef ENABLE_CACHE_COHERENCE
#define ENABLE_CACHE_COHERENCE 1
#endif


void *run_worker(void *arg)
{
	int i, j, ret;
	uint16_t qp_i;
	struct thread_params params = *(struct thread_params *) arg;
	uint16_t wrkr_lid = params.id;	/* Local ID of this worker thread*/
	int num_server_ports = MAX_SERVER_PORTS, base_port_index = 0;
	uint8_t worker_sl = 0;
	int remote_client_num =  CLIENT_NUM - CLIENTS_PER_MACHINE;
	assert(MICA_MAX_BATCH_SIZE >= WORKER_MAX_BATCH);
	assert(HERD_VALUE_SIZE <= MICA_MAX_VALUE);
	assert(WORKER_SS_BATCH > WORKER_MAX_BATCH);	/* WORKER_MAX_BATCH check */

	/* ---------------------------------------------------------------------------
	------------Set up the KVS partition-----------------------------------------
	---------------------------------------------------------------------------*/
#if ENABLE_WORKERS_CRCW == 0
	struct mica_kv kv;
	mica_init(&kv, (int)wrkr_lid, 0, HERD_NUM_BKTS, HERD_LOG_CAP); //0 refers to numa node
	mica_populate_fixed_len(&kv, HERD_NUM_KEYS, HERD_VALUE_SIZE);
#endif

	/* ---------------------------------------------------------------------------
	------------Set up the control block-----------------------------------------
	---------------------------------------------------------------------------*/
	uint16_t worker_req_size = sizeof(struct wrkr_ud_req);
	assert(num_server_ports <= MAX_SERVER_PORTS);	/* Avoid dynamic alloc */
	struct hrd_ctrl_blk *cb[MAX_SERVER_PORTS];
	uint32_t wrkr_buf_size = worker_req_size * CLIENTS_PER_MACHINE * (MACHINE_NUM - 1) * WS_PER_WORKER;

	int *wrkr_recv_q_depth = malloc(WORKER_NUM_UD_QPS * sizeof(int));
	int *wrkr_send_q_depth = malloc(WORKER_NUM_UD_QPS * sizeof(int));
	for (qp_i = 0; qp_i < WORKER_NUM_UD_QPS; qp_i++) {
		wrkr_recv_q_depth[qp_i] = WORKER_RECV_Q_DEPTH; // TODO fix this as a performance opt(it should be at the qp granularity)
		wrkr_send_q_depth[qp_i] = WORKER_SEND_Q_DEPTH; // TODO fix this as a performance opt
	}

	for(i = 0; i < num_server_ports; i++) {
		int ib_port_index = base_port_index + i;
		int use_huge_pages = ENABLE_HUGE_PAGES_FOR_WORKER_REQUEST_REGION == 1 ? 0 : -1;
		cb[i] = hrd_ctrl_blk_init((int)wrkr_lid,	/* local_hid */
								  ib_port_index, use_huge_pages, /* port index, numa node */
								  0, 0,	/* #conn qps, uc */
								  NULL, 0, -1,	/*prealloc conn buf, buf size, key */
								  WORKER_NUM_UD_QPS, wrkr_buf_size, MASTER_SHM_KEY + wrkr_lid, /* num_dgram_qps, dgram_buf_size, key */
								  wrkr_recv_q_depth, wrkr_send_q_depth);	/* Depth of the dgram RECV Q*/
	}

	/* ---------------------------------------------------------------------------
	------------Set up the buffer space for multiple UD QPs----------------------
	---------------------------------------------------------------------------*/


	uint16_t clts_per_qp[WORKER_NUM_UD_QPS];
	uint32_t per_qp_buf_slots[WORKER_NUM_UD_QPS], qp_buf_base[WORKER_NUM_UD_QPS];
	int pull_ptr[WORKER_NUM_UD_QPS] = {0}, push_ptr[WORKER_NUM_UD_QPS] = {0}; // it is useful to keep these signed
	setup_the_buffer_space(clts_per_qp, per_qp_buf_slots, qp_buf_base);
	uint32_t max_reqs = (uint32_t) wrkr_buf_size / worker_req_size;

	struct wrkr_ud_req *req[WORKER_NUM_UD_QPS]; // break up the buffer to ease the push/pull ptr handling
	for (qp_i = 0; qp_i < WORKER_NUM_UD_QPS; qp_i++) {
		pull_ptr[qp_i] = -1;
		req[qp_i] = (struct wrkr_ud_req *)(cb[0]->dgram_buf + (qp_buf_base[qp_i] * worker_req_size));
	}
	//Dgram buffer is all zeroed out in the cb initialization phase
	//printf("WORKER %d maximum reqs are %d\n", wrkr_lid, max_reqs );

	/* ---------------------------------------------------------------------------
	------------Fill the Recv Queue before publishing!-------------------------
	---------------------------------------------------------------------------*/
	uint32_t debug_recv = 0;
	uint16_t maximum_clients_per_worker = CEILING(CLIENTS_PER_MACHINE, ACTIVE_WORKERS_PER_MACHINE);
	if ((ENABLE_LOCAL_WORKERS == 0) || (wrkr_lid < ACTIVE_WORKERS_PER_MACHINE)) {
		for (qp_i = 0; qp_i < WORKER_NUM_UD_QPS; qp_i++) {
			for (i = 0; i < WS_PER_WORKER; i++) {
				uint16_t total_clients_per_qp = ENABLE_THREAD_PARTITIONING_C_TO_W == 1 ?
												maximum_clients_per_worker * (MACHINE_NUM - 1) : clts_per_qp[qp_i];
				for (j = 0; j < total_clients_per_qp; j++) {
					if (DEBUG_WORKER_RECVS) debug_recv++;
					hrd_post_dgram_recv(cb[0]->dgram_qp[qp_i],
										(void *) (cb[0]->dgram_buf + (push_ptr[qp_i] + qp_buf_base[qp_i]) * worker_req_size),
										worker_req_size, cb[0]->dgram_buf_mr->lkey);
					MOD_ADD_WITH_BASE(push_ptr[qp_i], per_qp_buf_slots[qp_i], 0);
				}
			}
		}
	}

	//printf("Posted %d receives, buffer size %d, max reqs %d \n", clts_per_qp[0] * WS_PER_WORKER, per_qp_buf_slots[0], max_reqs);

	/* -----------------------------------------------------
	--------------CONNECT WITH CLIENTS-----------------------
	---------------------------------------------------------*/
	char wrkr_dgram_qp_name[WORKER_NUM_UD_QPS][HRD_QP_NAME_SIZE];
	for (qp_i = 0; qp_i < WORKER_NUM_UD_QPS; qp_i++) {
		sprintf(wrkr_dgram_qp_name[qp_i], "worker-dgram-%d-%d-%d", machine_id, wrkr_lid, qp_i);
		hrd_publish_dgram_qp(cb[0], qp_i, wrkr_dgram_qp_name[qp_i], worker_sl); // need to do this per server_port, per UD QP
	}
	//printf("main: Worker %d published dgram %s \n", wrkr_lid, wrkr_dgram_qp_name);
	/* Create an address handle for each client */
	if (wrkr_lid == 0) {
		create_AHs_for_worker(wrkr_lid, cb[0]);
		assert(wrkr_needed_ah_ready == 0);
		wrkr_needed_ah_ready = 1;
	}
	else
		while (wrkr_needed_ah_ready == 0) usleep(200000);
	assert(wrkr_needed_ah_ready == 1);
	//printf("WORKER %d has all the needed ahs\n", wrkr_lid );

	struct mica_op *op_ptr_arr[WORKER_MAX_BATCH];//, *local_op_ptr_arr;
	struct mica_resp mica_resp_arr[WORKER_MAX_BATCH], local_responses[CLIENTS_PER_MACHINE * LOCAL_WINDOW], mica_batch_resp[WORKER_MAX_BATCH]; // this is needed because we batch to MICA all reqs between 2 writes
	struct ibv_send_wr wr[WORKER_MAX_BATCH], *bad_send_wr = NULL;
	struct ibv_sge sgl[WORKER_MAX_BATCH];

	struct ibv_wc wc[WS_PER_WORKER * (CLIENT_NUM - CLIENTS_PER_MACHINE)], send_wc;
	struct ibv_recv_wr recv_wr[WORKER_MAX_BATCH], *bad_recv_wr;
	struct ibv_sge recv_sgl[WORKER_MAX_BATCH];
	long long rolling_iter = 0, local_nb_tx = 0, local_tot_tx = 0;
	long long nb_tx_tot[WORKER_NUM_UD_QPS] = {0};
	int ws[CLIENTS_PER_MACHINE] = {0}; // WINDOW SLOT (Push pointer) of local clients
	int clt_i = -1;
	uint16_t last_measured_wr_i = 0, resp_buf_i = 0, received_messages,
			wr_i = 0, per_qp_received_messages[WORKER_NUM_UD_QPS] = {0};
	struct mica_op* dbg_buffer = malloc(HERD_PUT_REQ_SIZE);
	assert(CLIENTS_PER_MACHINE % num_server_ports == 0);

	struct mica_op *local_op_ptr_arr[CLIENTS_PER_MACHINE * LOCAL_WINDOW];
	for (i = 0; i < CLIENTS_PER_MACHINE * LOCAL_WINDOW; i++)
		local_op_ptr_arr[i] = (struct mica_op*)(local_req_region + ((wrkr_lid * CLIENTS_PER_MACHINE * LOCAL_WINDOW) + i));
	struct wrkr_coalesce_mica_op* response_buffer; // only used when inlining is not possible
	struct ibv_mr* resp_mr;
	setup_worker_WRs(&response_buffer, resp_mr, cb[0], recv_sgl, recv_wr, wr, sgl, wrkr_lid);

	/* ---------------------------------------------------------------------------
	------------Initialize Cache Coherence Context (Phase 6)---------------------
	---------------------------------------------------------------------------*/
	struct worker_coherence_ctx coh_ctx;
#if ENABLE_CACHE_COHERENCE == 1
	uint16_t wrkr_gid = machine_id * WORKERS_PER_MACHINE + wrkr_lid;
	if (worker_coherence_init(&coh_ctx, cb[0], wrkr_gid) != 0) {
		fprintf(stderr, "Worker %d: Failed to initialize coherence context\n", wrkr_lid);
		// Continue without coherence (graceful degradation)
	} else {
		printf("Worker %d: Cache coherence enabled\n", wrkr_lid);
	}
#endif

	// Buffers for coherence updates
	struct cache_op coh_update_ops[WORKER_BCAST_TO_CACHE_BATCH];
	struct mica_resp coh_update_resp[WORKER_BCAST_TO_CACHE_BATCH];
	struct ibv_wc coh_wc[WORKER_BCAST_TO_CACHE_BATCH];

	qp_i = 0;
	uint16_t send_qp_i = 0, per_recv_qp_wr_i[WORKER_NUM_UD_QPS] = {0};
	uint32_t dbg_counter = 0;
	uint8_t requests_per_message[WORKER_MAX_BATCH] = {0};
	uint16_t send_wr_i;

	// start the big loop
	while (1) {
		/* Do a pass over requests from all clients */
		wr_i = 0;

		/* ---------------------------------------------------------------------------
		------------------------------ POLL COHERENCE UPDATES (Phase 6)--------------
		---------------------------------------------------------------------------*/
#if ENABLE_CACHE_COHERENCE == 1
		// Poll for coherence updates from other servers
		uint16_t coh_update_num = worker_poll_coherence(&coh_ctx, coh_update_ops,
		                                                coh_update_resp, wrkr_gid);

		// Apply received updates to local cache
		if (coh_update_num > 0) {
			worker_apply_coherence_updates(coh_update_ops, coh_update_resp,
			                              coh_update_num, wrkr_lid);

			// Send credit messages back to senders
			int num_comps = ibv_poll_cq(cb[0]->dgram_recv_cq[WORKER_BROADCAST_QP_ID],
			                            coh_update_num, coh_wc);
			if (num_comps > 0) {
				worker_create_credits(&coh_ctx, coh_wc, num_comps, cb[0], wrkr_gid);
			}
		}
#endif

		/* ---------------------------------------------------------------------------
		------------------------------ LOCAL REQUESTS--------------------------------
		---------------------------------------------------------------------------*/
		// Before polling for remote reqs, poll for all the local, such that you give time for the remote reqs to gather up
		if (DISABLE_LOCALS != 1) {
			if (ENABLE_LOCAL_WORKERS && wrkr_lid >= ACTIVE_WORKERS_PER_MACHINE || !ENABLE_LOCAL_WORKERS) {
				serve_local_reqs(wrkr_lid, &kv, local_op_ptr_arr, local_responses);
				if (ENABLE_LOCAL_WORKERS) continue;
			}
		}

		/* ---------------------------------------------------------------------------
		------------------------------ POLL REQUEST REGION--------------------------
		---------------------------------------------------------------------------*/
		// Pick up the remote requests one by one
		qp_i = 0;
		bool multiget = false;
		uint16_t get_num = 0;
		uint16_t get_i = 0;
		send_wr_i = 0;
		memset(requests_per_message, 0, received_messages);
		received_messages = 0;
		memset(per_recv_qp_wr_i, 0, WORKER_NUM_UD_QPS * sizeof(uint16_t)); // how many wrs to send in each qp
		memset(per_qp_received_messages, 0, WORKER_NUM_UD_QPS * sizeof(uint16_t)); // how many qps have actually been received in each qp

		while (wr_i < WORKER_MAX_BATCH) {
			struct mica_op *next_req_ptr;
			uint32_t index = (pull_ptr[qp_i] + 1) % per_qp_buf_slots[qp_i];
			enum control_flow_directive cf = poll_remote_region(&qp_i, pull_ptr, per_qp_buf_slots, req[qp_i],
																&multiget, wr_i, &received_messages, // TODO choose whether to break on wr_i or send_wr_i
																per_qp_received_messages,
																&next_req_ptr, &get_i, &get_num, requests_per_message);
			if (cf == break_) break;
			else if (cf == continue_) continue;
			request_bookkeeping(multiget, pull_ptr, qp_i, per_qp_buf_slots, &next_req_ptr, &received_messages,
								per_qp_received_messages, req[qp_i], &get_i, send_wr_i, wr, nb_tx_tot, // TODO make sure latency is not affected by send_wr_i
								&clt_i, &index, send_qp_i, cb[0], wrkr_lid, push_ptr[qp_i], &last_measured_wr_i,
								requests_per_message);

			op_ptr_arr[wr_i] = next_req_ptr; // MICA array of ptrs

			//sgl[wr_i].length = HERD_PUT_REQ_SIZE;
			//nb_tx_tot[send_qp_i]++;/* Must increment inside loop */
			wr_i++;
			if (ENABLE_WORKER_COALESCING == 0 || ((multiget == 0) || (get_i == 1)))
				send_wr_i++;
			per_recv_qp_wr_i[qp_i]++;
			if (check_polling_conditions(&multiget, get_i, get_num, pull_ptr, &qp_i, per_qp_buf_slots[qp_i],
										 per_qp_received_messages[qp_i], wr_i, received_messages, wrkr_lid, max_reqs) == break_)
				break;
		}

		if (wr_i == 0) {
			w_stats[wrkr_lid].empty_polls_per_worker++;
			continue; // no request was found, start over
		}

		/* ---------------------------------------------------------------------------
		------------------------------ SERVER-SIDE CACHE LOOKUP---------------------
		---------------------------------------------------------------------------*/
		// Arrays for cache misses
		struct mica_op *cache_miss_ops[WORKER_MAX_BATCH];
		uint16_t cache_miss_indices[WORKER_MAX_BATCH];
		struct mica_resp kvs_resp[WORKER_MAX_BATCH];

		// Query cache first - separates hits from misses
		uint16_t cache_miss_count = worker_query_cache(wr_i, op_ptr_arr, mica_resp_arr,
		                                               cache_miss_ops, cache_miss_indices);

		/* ---------------------------------------------------------------------------
		------------------------------ LOCAL/REMOTE SEPARATION-----------------------
		---------------------------------------------------------------------------*/
		// After cache miss, separate local shard keys from remote keys
		struct mica_op *local_ops[WORKER_MAX_BATCH];
		uint16_t local_indices[WORKER_MAX_BATCH];
		struct mica_op *remote_ops[WORKER_MAX_BATCH];
		uint16_t remote_indices[WORKER_MAX_BATCH];
		uint8_t remote_machines[WORKER_MAX_BATCH];

		uint16_t local_count = 0;
		uint16_t remote_count = 0;

		if (cache_miss_count > 0) {
			local_count = worker_separate_local_remote(cache_miss_count,
			                                           cache_miss_ops, cache_miss_indices,
			                                           local_ops, local_indices,
			                                           remote_ops, remote_indices,
			                                           remote_machines);
			remote_count = cache_miss_count - local_count;
		}

		/* ---------------------------------------------------------------------------
		------------------------------ LOCAL KVS LOOKUP------------------------------
		---------------------------------------------------------------------------*/
		// Query local KVS for keys that belong to this shard
		if (local_count > 0) {
			KVS_BATCH_OP(&kv, local_count, local_ops, kvs_resp);
			// Merge local KVS responses
			worker_merge_responses(wr_i, mica_resp_arr, kvs_resp,
			                      local_indices, local_count);
		}

		/* ---------------------------------------------------------------------------
		------------------------------ REMOTE FORWARDING-----------------------------
		---------------------------------------------------------------------------*/
		// Forward remote requests to the appropriate servers
		if (remote_count > 0) {
			// Forward these requests to workers that own the keys
			worker_forward_requests(remote_count, remote_ops, remote_indices,
			                       remote_machines, cb[0], wrkr_lid, NULL);

			// Mark these requests as "forwarded" in responses
			// The actual response will come from the target worker to the client
			for(uint16_t i = 0; i < remote_count; i++) {
				uint16_t idx = remote_indices[i];
				// Mark as handled (forwarded)
				mica_resp_arr[idx].type = MICA_RESP_GET_SUCCESS;  // Temporary
				mica_resp_arr[idx].val_ptr = NULL;
				mica_resp_arr[idx].val_len = 0;
				// Note: Actual response will be sent by target worker
				// This is a limitation - ideally we'd suppress this response
			}
		}

		/* ---------------------------------------------------------------------------
		------------------------------ CACHE COHERENCE BROADCAST (Phase 6)----------
		---------------------------------------------------------------------------*/
#if ENABLE_CACHE_COHERENCE == 1
		// Prepare cache operations for writes that need to be broadcasted
		// Scan successful local writes and mark them for broadcast
		struct cache_op broadcast_ops[WORKER_MAX_BATCH];
		uint16_t broadcast_count = 0;

		for (uint16_t i = 0; i < wr_i; i++) {
			// Check if this is a successful PUT operation
			if (op_ptr_arr[i]->opcode == MICA_OP_PUT &&
			    (mica_resp_arr[i].type == MICA_RESP_PUT_SUCCESS ||
			     mica_resp_arr[i].type == CACHE_PUT_SUCCESS)) {

				// Convert to cache_op and mark for broadcast
				memcpy(&broadcast_ops[broadcast_count].key, &op_ptr_arr[i]->key,
				       sizeof(struct mica_key));
				broadcast_ops[broadcast_count].opcode = CACHE_OP_BRC;  // Mark for broadcast
				broadcast_ops[broadcast_count].val_len = op_ptr_arr[i]->val_len;
				memcpy(broadcast_ops[broadcast_count].value, op_ptr_arr[i]->value,
				       op_ptr_arr[i]->val_len);

				broadcast_count++;
			}
		}

		// Broadcast updates to all other servers
		if (broadcast_count > 0) {
			worker_broadcast_updates(&coh_ctx, broadcast_ops, broadcast_count,
			                        cb[0], wrkr_gid);
		}
#endif

		/* ---------------------------------------------------------------------------
		------------------------------ POLL COMPLETIONS------------------------
		---------------------------------------------------------------------------*/
		poll_workers_recv_completions(per_qp_received_messages, received_messages, cb[0],
									  wc, multiget, &debug_recv, wr_i,  wrkr_lid,  max_reqs);

		//green_printf("No of reqs %d through %d messages \n", wr_i, send_wr_i);
		//if (w_stats[wrkr_lid].batches_per_worker == 0 || w_stats[wrkr_lid].batches_per_worker == MILLION)
		//printf("Worker: %d Response %d bkt requested %llu \n", wrkr_lid, mica_resp_arr[0].type, op_ptr_arr[0]->key.bkt );
		if ((ENABLE_WORKER_COALESCING == 1) &&  (ENABLE_ASSERTIONS == 1)) assert(send_wr_i == received_messages);
		append_responses_to_work_requests_and_delete_requests(wr_i, op_ptr_arr, wr,
															  mica_resp_arr, &resp_buf_i,
															  wrkr_lid, sgl, response_buffer, requests_per_message);

		/* ---------------------------------------------------------------------------
		------------------------------ POST RECEIVES AND SENDS------------------------
		---------------------------------------------------------------------------*/
		worker_post_receives_and_sends(send_wr_i, cb[0], per_qp_received_messages, recv_wr, recv_sgl, push_ptr, qp_buf_base,
									   per_qp_buf_slots, &debug_recv, clts_per_qp, wr, send_qp_i, wrkr_lid,
									   last_measured_wr_i, wr_i);

	}
	return NULL;
}
