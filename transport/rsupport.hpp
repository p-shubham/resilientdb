#include <stdlib.h>
#include <infiniband/verbs.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <fstream>
#include <thread>
#include <bits/stl_map.h>
#include <iostream>
#include <assert.h>
#include <nanomsg/nn.h>
#include <nanomsg/pair.h>
#include <vector>
#include <set>
#include <iterator>
#include <string.h>
#include <unistd.h>

using namespace std;

#define g_total_node_cnt 5
#define TX_DEPTH 2048
#define PRIMARY_IB_PORT 1
#define KB 1024
#define MSG_SIZES MSG_SIZE_MAX / 2//520204 Bytes
#define CLIENT_REQ_NUM 5

#define CPE(val, msg, err_code)                    \
    if (val)                                       \
    {                                              \
        fprintf(stderr, msg);                      \
        fprintf(stderr, " Error %s \n", strerror( err_code )); \
        exit(err_code);                            \
    }

std::mutex mtx;
std::mutex accmtx;
//These are the memory regions
char **client_req_, **signed_req_area, **replica_mem_area, **exec_mem_area, *local_area, *cas_area, *read_area;

//Registered memory
struct ibv_mr *local_area_mr, *cas_area_mr, *read_area_mr, **signed_req_mr, **replica_mem_mr, **exec_mem_mr, **client_req_mr;

//Following are the functions to support RDMA operations:
union ibv_gid get_gid(struct ibv_context *context);
uint16_t get_local_lid(struct ibv_context *context);
static int poll_cq(struct ibv_cq *cq, int num_completions);
void create_qp(struct context *ctx);
void qp_to_init(struct context* ctx);
static void destroy_ctx(struct context *ctx);
static int tcp_exchange_qp_info();
int setup_buffers();
static int qp_to_rtr(struct ibv_qp *qp, struct context *ctx);
static int qp_to_rts(struct ibv_qp *qp, struct context *ctx);

//Standard Functions: Without polling
void post_recv(struct context *ctx, int num_recvs, int qpn, char  *local_addr, int local_key, int size);
void post_send(struct context *ctx, int qpn, char *local_addr, int local_key, int signal, int size);
void post_write(struct context *ctx, int qpn, char *local_addr, int local_key, uint64_t remote_addr, int remote_key, int signal, int size);
void post_read(struct context *ctx, int qpn, char *local_addr, int local_key, uint64_t remote_addr, int remote_key, int signal, int size);
void post_atomic(struct context *ctx, int qpn, char *local_addr, int local_key, unsigned long remote_addr, int remote_key, int64_t compare, int64_t swap, int signal, int size);
 
//APIs to be used for actual operations
void rdma_local_write(struct context *ctx, char* local_area, char* buf);
char* rdma_local_read(struct context *ctx, char* local_area, char* buf);
int rdma_remote_write(struct context *ctx, int dest, char *local_area, int lkey, unsigned long remote_buf, int rkey, int size);
int rdma_remote_read(struct context *ctx, int dest, char *local_area, int lkey, unsigned long remote_buf, int rkey, int size);
void rdma_atomic(struct ctrl_blk *ctx, int qpn, char *local_addr, int local_key, unsigned long remote_buf, int rkey, char compare, char swap);

struct qp_attr
{
    int id;
    int lid;
    int qpn;
    int psn;
};

struct stag
{
    uint32_t id;
    unsigned long buf[CLIENT_REQ_NUM];
	uint32_t rkey[g_total_node_cnt]; //
	uint32_t size;

};

//Stag for response and request
struct stag client_req_stag[g_total_node_cnt], signed_req_stag[g_total_node_cnt] ,replica_mem_stag[g_total_node_cnt], exec_mem_stag[g_total_node_cnt];

struct context
{
    struct ibv_device *ib_dev;
    struct ibv_context *context; //init
    struct ibv_pd *pd;           //init

    struct ibv_cq **cq;
    struct ibv_qp **qp;
    struct qp_attr *local_qp_attrs;
    struct qp_attr *remote_qp_attrs;

    struct ibv_ah *client_ah;

    struct ibv_send_wr wr;
    struct ibv_sge sgl;

    int num_conns;
    int is_client, id;
    int sock_port;
};

struct context *ctx;

// Initializes all the context related artifacts for the RDMA communication
static struct context *init_ctx()
{   
    struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
    srand48(getpid() * time(NULL));
	ctx = (context *) malloc(sizeof(struct context));
    ctx->id = g_node_id;
    ctx->local_qp_attrs = (struct qp_attr *) malloc(
			g_total_node_cnt * sizeof(struct qp_attr));
	ctx->remote_qp_attrs = (struct qp_attr *) malloc(
			g_total_node_cnt * sizeof(struct qp_attr));
    dev_list = ibv_get_device_list(NULL);
	CPE(!dev_list, "Failed to get IB devices list", 0);
	ib_dev = dev_list[0];
	//ib_dev = dev_list[0];
	CPE(!ib_dev, "IB device not found", 0);
    ctx->context = ibv_open_device(ib_dev);
    CPE(!ctx->context, "Couldn't get context", 0);
    ctx->pd = ibv_alloc_pd(ctx->context);
    CPE(!ctx->pd, "Couldn't allocate PD", 0);
    ctx->num_conns = g_total_node_cnt;
    ctx->sock_port = 1;
    create_qp(ctx);
    qp_to_init(ctx);

    return ctx;
}

void create_qp(struct context *ctx)
{
    int i;
    //Create connected queue pairs
    ctx->qp = (ibv_qp **) malloc(sizeof(ibv_qp) * ctx->num_conns);
    ctx->cq = (ibv_cq **) malloc(sizeof(ibv_cq) * ctx->num_conns);

    for (i = 0; i < ctx->num_conns; i++)
    {
        ctx->cq[i] = ibv_create_cq(ctx->context,
                                   TX_DEPTH + 1, NULL, NULL, 0);
        CPE(!ctx->cq[i], "Couldn't create RCQ", 0);
        // ctx->cq[i] = ibv_create_cq(ctx->context,
        //                            TX_DEPTH + 1, NULL, NULL, 0);
        // CPE(!ctx->cq[i], "Couldn't create SCQ", 0);

        struct ibv_qp_init_attr init_attr;
        memset(&init_attr, 0, sizeof(init_attr));
        init_attr.qp_type = IBV_QPT_RC;
        init_attr.sq_sig_all = 1;
        init_attr.send_cq = ctx->cq[i],
        init_attr.recv_cq = ctx->cq[i],
        init_attr.cap.max_send_wr = 2,
        init_attr.cap.max_recv_wr = 2,
        init_attr.cap.max_send_sge = 1,
        init_attr.cap.max_recv_sge = 1,
        // init_attr.cap.max_inline_data = 800; //Check this number
        // init_attr.qp_type = IBV_QPT_UC; // IBV_QPT_UC
        ctx->qp[i] = ibv_create_qp(ctx->pd, &init_attr);
        CPE(!ctx->qp[i], "Couldn't create connected QP", 0);
    }
}


union ibv_gid get_gid(struct ibv_context *context)
{
	union ibv_gid ret_gid;
	ibv_query_gid(context,PRIMARY_IB_PORT, 0, &ret_gid);

	// printf("GID: Interface id = %lld subnet prefix = %lld\n", 
	// 	(long long) ret_gid.global.interface_id,
	// 	(long long) ret_gid.global.subnet_prefix);
    
    cout << "Global Interface ID: " << ret_gid.global.interface_id << " Subnet Prefix: " << ret_gid.global.subnet_prefix << endl;
	
	return ret_gid;
}

uint16_t get_local_lid(struct ibv_context *context)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(context, PRIMARY_IB_PORT, &attr))
		return 0;

	return attr.lid;
}

int a[3] = {0, 1, 4};
int get_next_num(int thd_id){
    accmtx.lock();
    if(thd_id % g_rem_thread_cnt == 0){
        int nn = a[0];
        a[0]+=2;
        if (a[0] >= 4){
            a[0] = 0;
        }
        return nn;
    }
    else if(thd_id % g_rem_thread_cnt == 1){
        int nn = a[1];
        a[1] += 2;
        if (a[1] >= 4){
            a[1] = 1;
        }
        return nn;
    } else {
        return a[2];
    }
    accmtx.unlock();
}

int connect_ctx(int my_psn, struct qp_attr dest, struct ibv_qp* qp, int use_uc, int i)
{
	struct ibv_qp_attr *conn_attr;
    conn_attr = (struct ibv_qp_attr *)malloc(sizeof(struct ibv_qp_attr));
	conn_attr->qp_state			= IBV_QPS_RTR;
	conn_attr->path_mtu			= IBV_MTU_256;
	conn_attr->dest_qp_num		= dest.qpn;
	conn_attr->rq_psn			= dest.psn;
	conn_attr->ah_attr.dlid = dest.lid;
	conn_attr->ah_attr.sl = 0;
	conn_attr->ah_attr.src_path_bits = 0;
	conn_attr->ah_attr.port_num = PRIMARY_IB_PORT;
    conn_attr->min_rnr_timer = 12;

    // cout << "DEBUG: " << "Connecting context id: " << ctx->id << " to dest qpn: " << dest.qpn << endl;


	int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN
		| IBV_QP_RQ_PSN | IBV_QP_MIN_RNR_TIMER;
		
	if(!use_uc) {
		conn_attr->max_dest_rd_atomic = 1; // was 16 before
		conn_attr->min_rnr_timer = 12;
		rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	} 
	
	if (ibv_modify_qp(qp, conn_attr, rtr_flags)) {
		fprintf(stderr, "ibv_modify_qp to rtr failed: %s for node %d\n", strerror(errno), i);
		return 1;
	}
    // cout << "IBV_MODIFY_QP: RTR" << endl;
	memset(conn_attr, 0, sizeof(conn_attr));
	conn_attr->qp_state	    = IBV_QPS_RTS;
	conn_attr->sq_psn	    = my_psn;
	int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
	
	if(!use_uc) {
		conn_attr->timeout = 14;
		conn_attr->retry_cnt = 7;
		conn_attr->rnr_retry = 7;
		conn_attr->max_rd_atomic = 16;
		conn_attr->max_dest_rd_atomic = 16;
		rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
			  IBV_QP_MAX_QP_RD_ATOMIC;  
    }

   	if (ibv_modify_qp(qp, conn_attr, rts_flags)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}
    // cout << "IBV_MODIFY_QP: RTS" << endl;
	return 0;
}

void qp_to_init(struct context* ctx)
{
    struct ibv_qp_attr *attr;
    attr = (ibv_qp_attr *) malloc(sizeof(*attr));
    memset(attr, 0, sizeof(*attr));
    attr->qp_state = IBV_QPS_INIT;
    attr->pkey_index = 0;
    attr->port_num = PRIMARY_IB_PORT;
    attr->qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

    for(int i = 0; i < ctx->num_conns; i++){
        ibv_modify_qp(ctx->qp[i], attr,
                    IBV_QP_STATE |
                        IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT |
                        IBV_QP_ACCESS_FLAGS);
    }
}

static int poll_cq(struct ibv_cq *cq, int num_completions)
{

    // cout << "DEBUG: Polling for completions" << endl;
    // struct timespec end;
    // struct timespec start;
    struct ibv_wc *wc = (struct ibv_wc *)malloc(
        num_completions * sizeof(struct ibv_wc));
    int completions = 0, i = 0;
    // clock_gettime(CLOCK_REALTIME, &start);
    while (completions < num_completions)
    {   
        int new_comps = ibv_poll_cq(cq, num_completions - completions, wc);
        if (new_comps != 0)
        {
            completions += new_comps;
            for (i = 0; i < new_comps; i++)
            {
                if (wc[i].status < 0)
                {
                    perror("poll_recv_cq error");
                    exit(0);
                }
                // cout << "DEBUG: Status of CAS " << wc[i].opcode << endl;
                // cout << "DEBUG: Completed work request: " << wc[i].wr_id << endl;
            }
        }
        // clock_gettime(CLOCK_REALTIME, &end);
        // double seconds = (end.tv_sec - start.tv_sec) +
        //                  (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
        // if (seconds > 0.5)
        // {
        //     // printf("Polling Completed \n");
        //     // fflush(stdout);
        //     break;
        // }
    }
    // cout << "DEBUG: " << "Completions: " << completions << "Status: " << wc[0].imm_data << endl ;
    return 0;
}

// int setup_buffers(struct context* ctx){
//     int FLAGS = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
// 			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
//     resp_area = (char *) memalign(4096, MSG_SIZES);
//     resp_area_mr = ibv_reg_mr(ctx->pd, (char *) resp_area, MSG_SIZES, FLAGS);
//     memset((char *) resp_area, 0 , MSG_SIZES);

//     req_area = (char *)memalign(4096, MSG_SIZES);
//     req_area_mr = ibv_reg_mr(ctx->pd, (char *) req_area, MSG_SIZES, FLAGS);
//     memset((char *)req_area, 0 , MSG_SIZES);
// }

/*For Multiple Memory Registration*/

// int setup_client_buffer(){
//     int FLAGS = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
// 			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
//     client_req_ = (char **) malloc(g_total_node_cnt*sizeof(char *)); // TODO: indexSize
//     client_req_mr = (struct ibv_mr **) malloc(g_total_node_cnt*sizeof(struct ibv_mr *));
//     for(int i = 0; i < CLIENT_REQ_NUM;i++){
//         cout << "DEBUG MEM: CLIENT" << i << endl;
//         client_req_[i] = (char *) memalign(512, MSG_SIZES);
//         client_req_mr[i] = ibv_reg_mr(ctx->pd, (char *) client_req_[i], MSG_SIZES, FLAGS);
//         memset(client_req_[i], 0, MSG_SIZES);
//     }
//     return 0;
// }

int setup_buffers(){ // TODO: ctx is global variable
    int FLAGS = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
				IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    
    // Location for storing request signed by primary.
    signed_req_area = ( char **) malloc(g_total_node_cnt*sizeof(char *)); // TODO: indexSize / Brainstorm application of lock free Queues
    signed_req_mr = (struct ibv_mr **) malloc(g_total_node_cnt*sizeof(struct ibv_mr *));
    for(int i = 0; i < g_total_node_cnt;i++){ 
        cout << "DEBUG MEM: SIGNED" << i << endl;
        signed_req_area[i] = (char *) memalign(512, MSG_SIZES);
        signed_req_mr[i] = ibv_reg_mr(ctx->pd, (char *) signed_req_area[i], MSG_SIZES, FLAGS);
        memset(signed_req_area[i], 0 , MSG_SIZES);
    }

    // Location for storing request taken from Primary's signed area.
    replica_mem_area = (char **) malloc(g_total_node_cnt*sizeof(char *)); // TODO: should be g_rem_thread_cnt
    replica_mem_mr = (struct ibv_mr **) malloc(g_total_node_cnt*sizeof(struct ibv_mr *));
    for(int i = 0; i < g_total_node_cnt;i++){
        cout << "DEBUG MEM: REPLICA" << i << endl;
        replica_mem_area[i] = (char *) memalign(512, MSG_SIZES);
        replica_mem_mr[i] = ibv_reg_mr(ctx->pd, (char *) replica_mem_area[i], MSG_SIZES, FLAGS);
        memset(replica_mem_area[i], 0 , MSG_SIZES);
    }

    exec_mem_area = (char **) malloc(g_total_node_cnt*g_total_node_cnt*sizeof(char *)); //TODO: remove this
    exec_mem_mr = (struct ibv_mr **) malloc(g_total_node_cnt*g_total_node_cnt*sizeof(struct ibv_mr *));
    for(int i = 0; i < g_total_node_cnt;i++){
        cout << "DEBUG MEM: EXEC" << i << endl;
        exec_mem_area[i] = (char *) memalign(512, MSG_SIZES);
        exec_mem_mr[i] = ibv_reg_mr(ctx->pd, (char *) exec_mem_area[i], MSG_SIZES, FLAGS);
        memset(exec_mem_area[i], 0 , MSG_SIZES);
    }

    client_req_ = (char **) malloc(g_total_node_cnt*sizeof(char *)); // TODO: indexSize
    client_req_mr = (struct ibv_mr **) malloc(g_total_node_cnt*sizeof(struct ibv_mr *));
    for(int i = 0; i < g_total_node_cnt;i++){
        cout << "DEBUG MEM: CLIENT" << i << endl;
        client_req_[i] = (char *) memalign(512, MSG_SIZES);
        client_req_mr[i] = ibv_reg_mr(ctx->pd, (char *) client_req_[i], MSG_SIZES, FLAGS);
        memset(client_req_[i], 0, MSG_SIZES);
    }

    local_area = (char *) memalign(512, MSG_SIZES); // TODO: message size should be MAX_MSG_SIZE
    local_area_mr = ibv_reg_mr(ctx->pd, (char *) local_area, MSG_SIZES, FLAGS);
    memset(local_area, 0, MSG_SIZES);

    cas_area = (char *) memalign(512, MSG_SIZES);
    cas_area_mr = ibv_reg_mr(ctx->pd, (char *) cas_area, MSG_SIZES / 2, FLAGS);
    memset(cas_area, 0, MSG_SIZES);

    read_area = (char *) memalign(512, MSG_SIZES);
    read_area_mr = ibv_reg_mr(ctx->pd, (char *) read_area, MSG_SIZES / 2, FLAGS);
    memset(read_area, 0, MSG_SIZES);

    // setup_client_buffer();
    return 0;
}

/*
 *  *****Currently not in use*****
 *  Using connect_ctx function for this
 *  Changes Queue Pair status to 1. RTR (Ready to recieve) and RTS (Ready to send)
 *	QP status has to be RTR before changing it to RTS
*/
static int qp_to_rtr(struct ibv_qp *qp, struct context *ctx)
{
    struct ibv_qp_attr *attr;

    attr = (ibv_qp_attr *) malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state = IBV_QPS_RTR;
    attr->path_mtu = IBV_MTU_4096;
    attr->dest_qp_num = ctx->remote_qp_attrs->qpn;
    attr->rq_psn = ctx->remote_qp_attrs->psn;
    attr->max_dest_rd_atomic = 1;
    attr->min_rnr_timer = 0x12;
    attr->ah_attr.is_global = 0;
    attr->ah_attr.dlid = ctx->remote_qp_attrs->lid;
    attr->ah_attr.sl = 1; //may have to change
    attr->ah_attr.src_path_bits = 0;
    attr->ah_attr.port_num = ctx->sock_port;

    if(ibv_modify_qp(qp, attr,
                  IBV_QP_STATE |
                      IBV_QP_AV |
                      IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN |
                      IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC |
                      IBV_QP_MIN_RNR_TIMER) > 0){
                            fprintf(stderr, "ibv_modify_qp to rtr failed: %s\n", strerror(errno));
		                    return 1;
                      };

    free(attr);
    return 0;
}

static int qp_to_rts(struct ibv_qp *qp, struct context* ctx)
{
    qp_to_rtr(qp, ctx);
    struct ibv_qp_attr *attr;

    attr = (ibv_qp_attr *)malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state = IBV_QPS_RTS;
    attr->timeout = 14;
    attr->retry_cnt = 7;
    attr->rnr_retry = 7; /* infinite retry */
    attr->sq_psn = ctx->local_qp_attrs->psn;
    attr->max_rd_atomic = 1;

    if(ibv_modify_qp(qp, attr,
                  IBV_QP_STATE |
                      IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY |
                      IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC) > 0){
                            fprintf(stderr, "rts failed: %s\n", strerror(errno));
		                    return 1;
                      };

    free(attr);

    return 0;
}

void post_recv(struct context *ctx, int num_recvs, int qpn,  void  *local_addr, int local_key, int size)
{
	int ret;
	struct ibv_sge list;
	list.addr	= (uintptr_t) local_addr;
	list.length = size;
	list.lkey	= local_key;
	struct ibv_recv_wr wr;
    wr.next = NULL;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	struct ibv_recv_wr *bad_wr;
	ret = ibv_post_recv(ctx->qp[qpn], &wr, &bad_wr);
	CPE(ret, "Error posting recv\n", ret);
}

void post_send(struct context *ctx, int qpn,  void *local_addr, int local_key, int signal, int size)
{ 
	struct ibv_send_wr *bad_send_wr;
	ctx->sgl.addr = (uintptr_t) local_addr;
	ctx->sgl.lkey = local_key;	
	ctx->wr.opcode = IBV_WR_SEND;


	if(signal) {
		ctx->wr.send_flags |= IBV_SEND_SIGNALED;
	}

	ctx->wr.send_flags |= IBV_SEND_INLINE;	
 
	ctx->wr.sg_list = &ctx->sgl;
	ctx->wr.sg_list->length = size;
	ctx->wr.num_sge = 1;
    ctx->wr.wr_id = 1;

	int ret = ibv_post_send(ctx->qp[qpn], &ctx->wr, &bad_send_wr);
	
	CPE(ret, "ibv_post_send error", ret);
}

void post_write(struct context *ctx, int qpn, 
	 char *local_addr, int local_key, 
	uint64_t remote_addr, int remote_key, int signal, int size)
{
    struct ibv_send_wr *bad_send_wr;

    // cout << "DEBUG: " << "POSTING WRITE" << endl;


	ctx->sgl.addr = (uintptr_t) local_addr;
	ctx->sgl.lkey = local_key;
    ctx->sgl.length = size;

	ctx->wr.opcode = IBV_WR_RDMA_WRITE;
	if(signal){
		ctx->wr.send_flags |= IBV_SEND_SIGNALED;
	}
    
    ctx->wr.next = NULL;
	ctx->wr.sg_list = &ctx->sgl;
	
    ctx->wr.num_sge = 1;
	ctx->wr.wr.rdma.remote_addr = remote_addr;
	ctx->wr.wr.rdma.rkey = remote_key;

	int ret = ibv_post_send(ctx->qp[qpn], &ctx->wr, &bad_send_wr); //  &wrbatch[0]

    // cout << "RSUPPORT DEBUG: " << "POSTED WRITE ibv_post_send returned: " << ret << endl;

    if(ret){
    	perror("WRITE ERROR: ");
    }
	CPE(ret, "ibv_post_send error", ret);
}

void post_read(struct context *ctx, int qpn, 
	 char *local_addr, int local_key, 
	uint64_t remote_addr, int remote_key, int signal,int size)
{
    struct ibv_send_wr *bad_send_wr = NULL;
	ctx->sgl.addr = (uintptr_t) local_addr;
	ctx->sgl.lkey = local_key;
    ctx->sgl.length = size;
	
    memset(&ctx->wr, 0, sizeof(ctx->wr));

	ctx->wr.opcode = IBV_WR_RDMA_READ;
//	ctx->wr.send_flags = 0; for batching it is required
	if(signal) ctx->wr.send_flags |= IBV_SEND_SIGNALED;
    ctx->wr.next = NULL;
	ctx->wr.sg_list = &ctx->sgl;

	ctx->wr.num_sge = 1;
	ctx->wr.wr.rdma.remote_addr = remote_addr;
	ctx->wr.wr.rdma.rkey = remote_key;
		
	int ret = ibv_post_send(ctx->qp[qpn], &ctx->wr , &bad_send_wr); //&wrbatch[0]
	// cout << "DEBUG: ibv_post_send returned: " << ret << endl;
	CPE(ret, "ibv_post_send error", ret);

}


void post_atomic(struct context *ctx, int dest, 
	char *local_addr, int local_key, 
	unsigned long remote_addr, int remote_key, int64_t compare, int64_t swap, int signal, int size)
{
    struct ibv_send_wr *bad_send_wr = NULL;
	ctx->sgl.addr = (uintptr_t) local_addr;
	ctx->sgl.lkey = local_key;
	
    memset(&ctx->wr, 0, sizeof(ctx->wr));
	ctx->wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;

	if(signal){
		ctx->wr.send_flags |= IBV_SEND_SIGNALED;
	}
    ctx->wr.next = NULL;
	ctx->wr.sg_list = &ctx->sgl;
	ctx->wr.sg_list->length = size;
	ctx->wr.num_sge = 1;
    ctx->wr.wr.atomic.remote_addr = remote_addr;
    ctx->wr.wr.atomic.rkey = remote_key;
    ctx->wr.wr.atomic.compare_add = compare;
    ctx->wr.wr.atomic.swap = swap;
	int ret = ibv_post_send(ctx->qp[dest], &ctx->wr, &bad_send_wr); //  &wrbatch[0]
    // cout << "DEBUG: Atomic Send ret: " << ret << endl;
	CPE(ret, "ibv_post_send error", ret);
}

static void detroy_ctx(struct context *ctx)
{
    if (ctx->qp)
    {
        free(ctx->qp);
    }
    if (ctx->cq)
    {
        free(ctx->cq);
    }
    // if (ctx->scq)
    // {
    //     free(ctx->scq);
    // }
    if (ctx->context)
    {
        ibv_close_device(ctx->context);
    }
    //free memory regions
    free(client_req_);
    free(client_req_mr);
    free(client_req_);
    free(replica_mem_area);
    free(replica_mem_mr);
    free(exec_mem_area);
    free(exec_mem_mr);
    free(signed_req_area);
    free(signed_req_mr);
}
void print_stag(struct stag st)
{
	fflush(stdout);
    for(int i = 0; i < g_total_node_cnt; i++){
	    printf("Stag: \t id: %u, buf_region for node %d: %lu, rkey for node %d: %u, size: %u\n", st.id , i,st.buf[i], i,st.rkey[i], st.size);
    }
}

int rdma_send(struct context *ctx, int qpn,  void *local_addr, int local_key, int signal, int size){
    post_send(ctx, qpn, local_addr, local_key,signal,size);
	poll_cq(ctx->cq[qpn], 1);
    // cout << "Send queue polled" << endl;
    return size;
}

int rdma_recv(struct context *ctx, int num_recvs, int qpn,  void *local_addr, int local_key, int size){
    post_recv(ctx, num_recvs, qpn, local_addr, local_key,size);
    poll_cq(ctx->cq[qpn], num_recvs);
    // cout << "Recieve queue polled" << endl;
    return 0;
}

void rdma_local_write(struct context *ctx, void* local_area, void* buf){
	strcpy((char *)local_area, (char *)buf);
}
int rdma_remote_write(struct context *ctx, int dest, char *local_area, int lkey, unsigned long remote_buf, int rkey, int size){
	post_write(ctx, dest, local_area, lkey, remote_buf, rkey, 0, size);
    // return 1;
	return poll_cq(ctx->cq[dest], 1);
}
void* rdma_local_read(struct context *ctx, void* local_area){
    return local_area;
}
int rdma_remote_read(struct context *ctx, int dest, char *local_area, int lkey, unsigned long remote_buf, int rkey, int size){
    // cout << "DEBUG: Reading remote" << endl;
    post_read(ctx, dest, local_area, lkey, remote_buf, rkey, 1, size);
    // cout << "DEBUG: Polling for completions" << endl;
    return poll_cq(ctx->cq[dest], 1);
}

bool rdma_cas(struct context *ctx, int dest, char *local_addr, int local_key, unsigned long remote_addr, int remote_key, char compare, char swap){
    post_atomic(ctx, dest, local_addr, local_key, remote_addr, remote_key, compare, swap, 1, 8);
    poll_cq(ctx->cq[dest], 1);
    return (int)local_addr[0] == (int)compare;
}

bool local_cas(char* a, int c, int s){
    return __sync_bool_compare_and_swap(a, c, s);
}