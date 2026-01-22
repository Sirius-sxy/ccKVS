#include "cache.h"
#include "util.h"
#include <getopt.h>

//Global Vars
uint8_t protocol;
optik_lock_t kv_lock;
uint32_t* latency_counters;
struct latency_counters latency_count;
struct mica_op *local_req_region;
struct client_stats c_stats[CLIENTS_PER_MACHINE];
struct worker_stats w_stats[WORKERS_PER_MACHINE];
atomic_char local_recv_flag[WORKERS_PER_MACHINE][CLIENTS_PER_MACHINE][64]; //false sharing problem -- fixed with padding
struct remote_qp remote_wrkr_qp[WORKER_NUM_UD_QPS][WORKER_NUM];
struct remote_qp remote_clt_qp[CLIENT_NUM][CLIENT_UD_QPS];
atomic_char clt_needed_ah_ready, wrkr_needed_ah_ready;

#if ENABLE_WORKERS_CRCW == 1
struct mica_kv kv;
#endif



int main(int argc, char *argv[])
{
	printf("SERVER MODE: Starting ccKVS server (workers only)\n");

	// Worker-specific assertions
	assert(WORKER_SEND_Q_DEPTH > WORKER_MAX_BATCH); // Worker responses
	assert(ENABLE_MULTI_BATCHES == 0 || CLIENT_SEND_REM_Q_DEPTH > MAX_OUTSTANDING_REQS);
	assert(LOCAL_WINDOW <= MICA_MAX_BATCH_SIZE);
	assert(WORKER_MAX_BATCH <= MICA_MAX_BATCH_SIZE);
	assert(WS_PER_WORKER < 256);
	assert(CLIENTS_PER_MACHINE >= WORKER_NUM_UD_QPS);
	assert(sizeof(struct mica_op) > HERD_PUT_REQ_SIZE);
	assert(sizeof(struct ud_req) == UD_REQ_SIZE);
	assert(sizeof(struct mica_op) == MICA_OP_SIZE);
	assert(sizeof(struct mica_key) == KEY_SIZE);
	assert(sizeof(struct extended_cache_op) <= sizeof(struct wrkr_ud_req) - GRH_SIZE);
	if (WORKER_HYPERTHREADING) assert(WORKERS_PER_MACHINE <= VIRTUAL_CORES_PER_SOCKET);
	assert(EXTRA_WORKER_REQ_BYTES >= 0);
	assert(WORKER_REQ_SIZE <= sizeof(struct wrkr_ud_req));
	assert(BASE_VALUE_SIZE % pow2roundup(SHIFT_BITS) == 0);
	if ((ENABLE_COALESCING == 1) && (DESIRED_COALESCING_FACTOR < MAX_COALESCE_PER_MACH))
		assert(ENABLE_WORKER_COALESCING == 0);

	cyan_printf("Size of worker req: %d, extra bytes: %d, ud req size: %d minimum worker"
				" req size %d, actual size of req_size %d, extended cache ops size %d  \n",
			WORKER_REQ_SIZE, EXTRA_WORKER_REQ_BYTES, UD_REQ_SIZE, MINIMUM_WORKER_REQ_SIZE,
			sizeof(struct wrkr_ud_req), sizeof(struct extended_cache_op));
	yellow_printf("Size of worker send req: %d, expected size %d  \n",
				  sizeof(struct wrkr_coalesce_mica_op), WORKER_SEND_BUFF_SIZE);

	int i, c;
	is_master = -1; is_client = 0; // SERVER: is_client = 0
	int num_threads = -1;
	int  postlist = -1, update_percentage = -1;
	int base_port_index = -1, num_server_ports = -1, num_client_ports = -1;
	is_roce = -1; machine_id = -1;
	remote_IP = (char *)malloc(16 * sizeof(char));

	struct thread_params *param_arr;
	pthread_t *thread_arr;

	static struct option opts[] = {
			{ .name = "machine-id",			.has_arg = 1, .val = 'm' },
			{ .name = "is-roce",			.has_arg = 1, .val = 'r' },
			{ 0 }
	};

	/* Parse and check arguments */
	while(1) {
		c = getopt_long(argc, argv, "m:r:", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 'm':
				machine_id = atoi(optarg);
				break;
			case 'r':
				is_roce = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				assert(false);
		}
	}

	/* Launch worker threads */
	assert(machine_id < MACHINE_NUM && machine_id >=0);
	num_threads = WORKERS_PER_MACHINE;

	param_arr = malloc(num_threads * sizeof(struct thread_params));
	thread_arr = malloc((WORKERS_PER_MACHINE + 1) * sizeof(pthread_t));
	local_req_region = (struct mica_op *)malloc(WORKERS_PER_MACHINE * CLIENTS_PER_MACHINE * LOCAL_WINDOW * sizeof(struct mica_op));
	memset((struct client_stats*) c_stats, 0, CLIENTS_PER_MACHINE * sizeof(struct client_stats));
	memset((struct worker_stats*) w_stats, 0, WORKERS_PER_MACHINE * sizeof(struct worker_stats));

	int j, k;
	for (i = 0; i < WORKERS_PER_MACHINE; i++)
		for (j = 0; j < CLIENTS_PER_MACHINE; j++) {
			for (k = 0; k < LOCAL_REGIONS; k++)
				local_recv_flag[i][j][k] = 0;
			for (k = 0; k < LOCAL_WINDOW; k++) {
				int offset = OFFSET(i, j, k);
				local_req_region[offset].opcode = 0;
			}
		}

	clt_needed_ah_ready = 0;
	wrkr_needed_ah_ready = 0;

	// SERVER-SIDE CACHE: Initialize cache for workers
	// Each server has symmetric cache with hottest keys
	cache_init(WORKERS_PER_MACHINE, CLIENTS_PER_MACHINE, (uint8_t)machine_id);

#if ENABLE_WORKERS_CRCW == 1
	mica_init(&kv, 0, 0, HERD_NUM_BKTS, HERD_LOG_CAP);
	/* Use partitioned populate - each server only loads its own slots */
	mica_populate_fixed_len_partitioned(&kv, HERD_NUM_KEYS, HERD_VALUE_SIZE, (uint8_t)machine_id);
	optik_init(&kv_lock);
#endif

	pthread_attr_t attr;
	cpu_set_t cpus_w;
	pthread_attr_init(&attr);
	int occupied_cores[TOTAL_CORES] = { 0 };

	// Spawn only workers
	for(i = 0; i < WORKERS_PER_MACHINE; i++) {
		param_arr[i].id = i;
		int w_core = pin_worker(i);
		green_printf("Creating worker thread %d at core %d \n", param_arr[i].id, w_core);
		CPU_ZERO(&cpus_w);
		CPU_SET(w_core, &cpus_w);
		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus_w);
		pthread_create(&thread_arr[i], &attr, run_worker, &param_arr[i]);
		occupied_cores[w_core] = 1;
	}

	// Join worker threads
	for(i = 0; i < WORKERS_PER_MACHINE; i++)
		pthread_join(thread_arr[i], NULL);

	return 0;
}
