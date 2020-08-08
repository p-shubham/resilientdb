#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>

using namespace std;

#define NODE_CNT 5
#define TX_DEPTH 2048
#define PRIMARY_IB_PORT 1
#define KB 1024

#define CPE(val, msg, err_code)                    \
    if (val)                                       \
    {                                              \
        fprintf(stderr, msg);                      \
        fprintf(stderr, " Error %s \n", strerror( err_code )); \
        exit(err_code);                            \
    }

//These are the memory regions
volatile int64_t *req_area, *resp_area, *local_read;

//Registered memory
struct ibv_mr *req_area_mr, *resp_area_mr, *local_read_mr;


//Following are the functions to support RDMA operations:
union ibv_gid get_gid(struct ibv_context *context);
uint16_t get_local_lid(struct ibv_context *context);
static int poll_cq(struct ibv_cq *cq, int num_completions);
void create_qp(struct context *ctx);
void qp_to_init(struct context* ctx);
static void destroy_ctx(struct context *ctx);
static int tcp_exchange_qp_info();
int setup_buffers(struct context *ctx);
static int qp_to_rtr(struct ibv_qp *qp, struct context *ctx);
static int qp_to_rts(struct ibv_qp *qp, struct context *ctx);
void post_recv(struct context *ctx, int num_recvs, int qpn, volatile int64_t  *local_addr, int local_key, int size);
void post_send(struct context *ctx, int qpn, volatile int64_t *local_addr, int local_key, int signal, int size);


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
    unsigned long buf;
	uint32_t rkey;
	uint32_t size;

} node_stags[NODE_CNT];

//Stag for response and request
struct stag req_area_stag[NODE_CNT], resp_area_stag[NODE_CNT];

struct context
{
    struct ibv_device *ib_dev;
    struct ibv_context *context; //init
    struct ibv_pd *pd;           //init

    struct ibv_cq **rcq;
    struct ibv_cq **scq;
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

static struct context *init_ctx(struct context *ctx, struct ibv_device *ib_dev)
{
    ctx->context = ibv_open_device(ib_dev);
    CPE(!ctx->context, "Couldn't get context", 0);

    ctx->pd = ibv_alloc_pd(ctx->context);
    CPE(!ctx->pd, "Couldn't allocate PD", 0);

    ctx->num_conns = NODE_CNT;

    create_qp(ctx);
    qp_to_init(ctx);

    return ctx;
}

void create_qp(struct context *ctx)
{
    int i;
    //Create connected queue pairs
    ctx->qp = (ibv_qp **) malloc(sizeof(int *) * ctx->num_conns);
    ctx->rcq = (ibv_cq **) malloc(sizeof(int *) * ctx->num_conns);
    ctx->scq = (ibv_cq **) malloc(sizeof(int *) * ctx->num_conns);


    for (i = 0; i < ctx->num_conns; i++)
    {
        ctx->rcq[i] = ibv_create_cq(ctx->context,
                                   TX_DEPTH + 1, NULL, NULL, 0);
        CPE(!ctx->rcq[i], "Couldn't create RCQ", 0);
        ctx->scq[i] = ibv_create_cq(ctx->context,
                                   TX_DEPTH + 1, NULL, NULL, 0);
        CPE(!ctx->scq[i], "Couldn't create SCQ", 0);

        struct ibv_qp_init_attr init_attr;
        init_attr.send_cq = ctx->scq[i],
        init_attr.recv_cq = ctx->rcq[i],
        init_attr.cap.max_send_wr = TX_DEPTH,
        init_attr.cap.max_recv_wr = TX_DEPTH,
        init_attr.cap.max_send_sge = 1,
        init_attr.cap.max_recv_sge = 1,
        init_attr.cap.max_inline_data = 800; //Check this number
        init_attr.qp_type = IBV_QPT_UC; // IBV_QPT_UC
        ctx->qp[i] = ibv_create_qp(ctx->pd, &init_attr);
        CPE(!ctx->qp[i], "Couldn't create connected QP", 0);
    }
}


union ibv_gid get_gid(struct ibv_context *context)
{
	union ibv_gid ret_gid;
	ibv_query_gid(context,PRIMARY_IB_PORT, 0, &ret_gid);

	printf("GID: Interface id = %lld subnet prefix = %lld\n", 
		(long long) ret_gid.global.interface_id, 
		(long long) ret_gid.global.subnet_prefix);
	
	return ret_gid;
}

uint16_t get_local_lid(struct ibv_context *context)
{
	struct ibv_port_attr attr;

	if (ibv_query_port(context, PRIMARY_IB_PORT, &attr))
		return 0;

	return attr.lid;
}


int connect_ctx(struct context *ctx, int my_psn, struct qp_attr dest, struct ibv_qp* qp, int use_uc)
{
	struct ibv_qp_attr *conn_attr;
    conn_attr = (struct ibv_qp_attr *)malloc(sizeof(struct ibv_qp_attr));
	conn_attr->qp_state			= IBV_QPS_RTR;
	conn_attr->path_mtu			= IBV_MTU_4096;
	conn_attr->dest_qp_num		= dest.qpn;
	conn_attr->rq_psn			= dest.psn;
	conn_attr->ah_attr.dlid = dest.lid;
	conn_attr->ah_attr.sl = 0;
	conn_attr->ah_attr.src_path_bits = 0;
	conn_attr->ah_attr.port_num = PRIMARY_IB_PORT;

	int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN
		| IBV_QP_RQ_PSN;
		
	if(!use_uc) {
		conn_attr->max_dest_rd_atomic = 16;
		conn_attr->min_rnr_timer = 12;
		rtr_flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	} 
	
	if (ibv_modify_qp(qp, conn_attr, rtr_flags)) {
		fprintf(stderr, "send failed: %s\n", strerror(errno));
		return 1;
	}
    cout << "IBV_MODIFY_QP: RTR" << endl;
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
    cout << "IBV_MODIFY_QP: RTS" << endl;
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
    attr->qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;

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
    struct timespec end;
    struct timespec start;
    struct ibv_wc *wc = (struct ibv_wc *)malloc(
        num_completions * sizeof(struct ibv_wc));
    int completions = 0, i = 0;
    clock_gettime(CLOCK_REALTIME, &start);
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
            }
        }
        clock_gettime(CLOCK_REALTIME, &end);
        double seconds = (end.tv_sec - start.tv_sec) +
                         (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
        if (seconds > 0.5)
        {
            // printf("Polling Completed \n");
            // fflush(stdout);
            break;
        }
    }
}

int setup_buffers(struct context* ctx){
    int FLAGS = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    resp_area = (int64_t *) memalign(4096, 64 * KB * sizeof(int64_t));
    resp_area_mr = ibv_reg_mr(ctx->pd, (int64_t *) resp_area, 64 * KB * sizeof(int64_t), FLAGS);
    memset((int64_t *) resp_area, 0 , 64 * KB * sizeof(int64_t));

    req_area = (int64_t *)memalign(4096, 64 * KB * sizeof(int64_t));
    req_area_mr = ibv_reg_mr(ctx->pd, (int64_t *) req_area, 64 * KB * sizeof(int64_t), FLAGS);
    memset((int64_t *)req_area, 0 , 64 * KB * sizeof(int64_t));
}

/*For Multiple Memory Registration
int setup_buffers(struct context* ctx){
    int FLAGS = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
				IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    
    resp_area = (volatile int64_t **) malloc(NODE_CNT*sizeof(int64_t *));
    resp_area_mr = (struct ibv_mr **) malloc(NODE_CNT*sizeof(struct ibv_mr *));
    for(int i = 0; i <NODE_CNT;i++){
        resp_area[i] = (int64_t *) memalign(4096, 64 * KB * sizeof(int64_t));
        resp_area_mr[i] = ibv_reg_mr(ctx->pd, (int64_t *) resp_area[i], 64 * KB * sizeof(int64_t), FLAGS);
        memset((int64_t *) resp_area[i], 0 , 64 * KB * sizeof(int64_t));
    }
    cout << "resp_area_mr set" << endl;
    req_area = (volatile int64_t **) malloc(NODE_CNT*sizeof(int64_t *));
    req_area_mr = (struct ibv_mr **) malloc(NODE_CNT*sizeof(struct ibv_mr *));
    for(int i = 0; i <NODE_CNT; i++){
        req_area[i] = (int64_t *)memalign(4096, 64 * KB * sizeof(int64_t));
        req_area_mr[i] = ibv_reg_mr(ctx->pd, (int64_t *) resp_area[i], 64 * KB * sizeof(int64_t), FLAGS);
        memset((int64_t *)req_area[i], 0 , 64 * KB * sizeof(int64_t));
    }
    cout << "req_area_mr set" << endl;
}
*/
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
    attr->min_rnr_timer = 12;
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
                            fprintf(stderr, "send failed: %s\n", strerror(errno));
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

void post_recv(struct context *ctx, int num_recvs, int qpn, volatile int64_t  *local_addr, int local_key, int size)
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
	int i;
	ret = ibv_post_recv(ctx->qp[qpn], &wr, &bad_wr);
	CPE(ret, "Error posting recv\n", ret);
}

void post_send(struct context *ctx, int qpn, volatile int64_t *local_addr, int local_key, int signal, int size)
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

	int ret = ibv_post_send(ctx->qp[qpn], &ctx->wr, &bad_send_wr);
	
	CPE(ret, "ibv_post_send error", ret);
}

static void detroy_ctx(struct context *ctx)
{
    if (ctx->qp)
    {
        free(ctx->qp);
    }
    if (ctx->rcq)
    {
        free(ctx->rcq);
    }
    if (ctx->scq)
    {
        free(ctx->scq);
    }
    if (ctx->context)
    {
        ibv_close_device(ctx->context);
    }
    free((void *)resp_area);
    free((void *) req_area);
    free(resp_area_mr);
    free(req_area_mr);
}
void print_stag(struct stag st)
{
	fflush(stdout);
	printf("Stag: \t id: %u, buf: %lu, rkey: %u, size: %u\n", st.id ,st.buf, st.rkey, st.size); 
}

int rdma_send(struct context *ctx, int qpn, volatile int64_t *local_addr, int local_key, int signal, int size){
    post_send(ctx, qpn, local_addr, local_key,signal,size);
	//poll_cq(ctx->scq[qpn], 1);
    return size;
}

int rdma_recv(struct context *ctx, int num_recvs, int qpn, volatile int64_t  *local_addr, int local_key, int size){
    post_recv(ctx, num_recvs, qpn, local_addr, local_key,size);
    poll_cq(ctx->rcq[qpn], num_recvs);
}

// struct application_data{
// 	int ib_port;
// 	unsigned size;
// 	int tx_depth;
// 	int sockfd;
// 	char *nodename;
// 	struct connection_attr local_con_info;
// 	struct connection_attr *remote_con_info;
// };

// static struct application_data* init_application_data(char r_u[21], int ib_port, int size, int tx_depth
// 								,char* name
// 								,struct connection_attr* remote_qp
// 								,struct ibv_device *dev){

//                                     struct application_data *temp;
// 									temp = (struct temp *) malloc(sizeof *temp);
// 									temp->ib_dev = dev;
// 									strncpy(temp->recv_url, r_u, 21);
// 									temp->ib_port = ib_port;
// 									temp->size = size;
// 									temp->tx_depth = tx_depth;
// 									temp->nodename = name;
// 									temp->remote_con_info = remote_qp;
//                                     return temp;
//                                 }

// static struct context* init_ctx(struct application_data* data){
//     struct context *ctx;
//     struct ibv_device **dev_list;
//     ctx = (struct context *) malloc(sizeof *ctx);
//     memset(ctx, 0, sizeof(&ctx));
//     ctx->size = data->size;
//     ctx->tx_depth = data->tx_depth;
//     posix_memalign(&ctx->buf ,sysconf(_SC_PAGESIZE), ctx->size * 2);
//     memset(ctx->buf, 0, ctx->size * 2);
//     dev_list = ibv_get_device_list(NULL);
//     if(!dev_list){
//         printf("Fatal: Device List\n");
//         return 0;
//     }
// 	data->ib_dev = dev_list[0];
//     ctx->context = ibv_open_device(data->ib_dev);
//     if(!ctx->context){
//         printf("Fatal: Context Error\n");
//         return 0;
//     }
//     ctx->pd = ibv_alloc_pd(ctx->context);
//     if(!ctx->pd){
//         printf("Fatal: PD Error\n");
//         return 0;
//     }
//     ctx->mr = ibv_reg_mr(ctx->pd,ctx->buf, ctx->size * 2
//                         ,IBV_ACCESS_REMOTE_WRITE
//                         | IBV_ACCESS_LOCAL_WRITE
//                         | IBV_ACCESS_REMOTE_READ);
//     if(!ctx->mr){
//         printf("Fatal: MR\n");
//         return 0;
//     }
//     ctx->ch = ibv_create_comp_channel(ctx->context);
//     if(!ctx->ch){
//         printf("Fatal: CH\n");
//         return 0;
//     }
//     //Check Parameters
//     ctx->rcq = ibv_create_cq(ctx->context, 1, NULL
//                 ,NULL, 0);
//     if(!ctx->rcq){
//         printf("Fatal: RCQ\n");
//         return 0;
//     }
//     ctx->scq = ibv_create_cq(ctx->context, ctx->tx_depth, NULL
//                 ,NULL, 0);
//     if(!ctx->scq){
//         printf("Fatal: SCQ\n");
//         return 0;
//     }

//     struct ibv_qp_init_attr qp_init_attr = {
//         .send_cq = ctx->scq,
//         .recv_cq = ctx->rcq,
//         .qp_type = IBV_QPT_RC,
//         .cap = {
//             .max_send_wr = ctx->tx_depth,
// 			.max_recv_wr = 1,
// 			.max_send_sge = 1,
// 			.max_recv_sge = 1,
// 			.max_inline_data = 0
//         }
//     };
//     ctx->qp = ibv_create_qp(ctx->pd, &qp_init_attr);
//     if(!ctx->qp){
//         printf("Fatal: QP\n");
//         return 0;
//     }
//     qp_to_init(ctx->qp, data);
//     return ctx;
// }

// static int tcp_exchange_qp_info(struct application_data *data)
// {
//     char msg[sizeof("0000:000000:000000:00000000:0000000000000000")];
//     int parsed;
//     int rc;
//     struct connection_attr *local = &data->local_con_info;
//     sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx",
//             local->lid, local->qpn, local->psn, local->rkey, local->vaddr);
//     while (rc != sizeof(msg))
//     {
//         rc = nn_send(data->sockfd, msg, sizeof(msg), NN_DONTWAIT);
//     }
//     if (rc != sizeof(msg))
//     {
//         rc = nn_recv(data->sockfd, msg, sizeof(msg), NN_DONTWAIT);
//     }

//     if (!data->remote_con_info)
//     {
//         free(data->remote_con_info);
//     }
//     data->remote_con_info = malloc(sizeof(struct connection_attr));
//     struct connection_attr *remote = data->remote_con_info;
//     parsed = sscanf(msg, "%x:%x:%x:%x:%Lx",
//                     &remote->lid, &remote->qpn, &remote->psn, &remote->rkey, &remote->vaddr);
//     if (parsed != 5)
//     {
//         printf("Error in Parsing\n");
//         free(data->remote_con_info);
//         return -1;
//     }
//     return 1;
// }