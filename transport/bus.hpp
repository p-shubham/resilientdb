#include "rsupport.hpp"
#include "nn.hpp"

using namespace std;
using namespace nn;
#define sz_n sizeof(struct stag)
#define S_QPA sizeof(struct qp_attr)

char **ifaddr;
std::map<int, int> send_sockets, recv_sockets;
std::map<int, stag> req_stags, resp_stags;

void print_qp_attr(struct qp_attr dest)
{
	fflush(stdout);
	printf("\t%d %d %d\n", dest.lid, dest.qpn, dest.psn);
}

uint64_t get_port_id(uint64_t src_node_id, uint64_t dest_node_id)
{
    uint64_t port_id = 20000;
    port_id += NODES_CNT * dest_node_id;
    port_id += src_node_id;
    return port_id;
}
void rread_ifconfig(const char *ifaddr_file)
{
    ifaddr = new char *[NODES_CNT];

    uint64_t cnt = 0;
    printf("Reading ifconfig file: %s\n", ifaddr_file);
    ifstream fin(ifaddr_file);
    string line;
    while (getline(fin, line))
    {
        //memcpy(ifaddr[cnt],&line[0],12);
        ifaddr[cnt] = new char[line.length() + 1];
        strcpy(ifaddr[cnt], &line[0]);
        printf("%ld: %s\n", cnt, ifaddr[cnt]);
        cnt++;
    }
    assert(cnt == NODES_CNT);
}

void recv_thread(int id, struct context *ctx)
{
    // cout << "In recv" << endl;
    int count = 0, i = 0;
    set<int> counter;
    while (count <= NODES_CNT - 1)
    {
        if (i == id || counter.count(i) > 0)
        {
            i++;
            continue;
        }
        if (i > NODES_CNT - 1)
        {
            i = 0;
            continue;
        }
        int sock = recv_sockets.find(i)->second;
        int r = nn_recv(sock, &resp_area_stag[i], sizeof(struct stag), 1);
        if (r > 1)
        {
            count++;
            counter.insert(i);
            // cout << "Recieved from " << i << endl;
        }
        i++;
        if (counter.size() == NODES_CNT - 1)
        {
            break;
        }
    }
    // cout << "Rec: res_area_stags" << endl;
    count = 0;
    i = 0;
    counter.clear();
    while (count <= NODES_CNT - 1)
    {
        if (i == id || counter.count(i) > 0)
        {
            i++;
            continue;
        }
        if (i > NODES_CNT - 1)
        {
            i = 0;
            continue;
        }
        int sock = recv_sockets.find(i)->second;
        int r = nn_recv(sock, &req_area_stag[i], sizeof(struct stag) + 1, 1);
        if (r > 1)
        {
            count++;
            counter.insert(i);
            // cout << "Recieved from " << i << endl;
        }
        i++;
        if (counter.size() == NODES_CNT - 1)
        {
            break;
        }
    }
    // cout << "Rec: req_area_stags" << endl;
    count = 0;
    i = 0;
    counter.clear();
    while (count <= NODES_CNT - 1)
    {
        if (i == id || counter.count(i) > 0)
        {
            i++;
            continue;
        }
        if (i > NODES_CNT - 1)
        {
            i = 0;
            continue;
        }
        int sock = recv_sockets.find(i)->second;
        if(nn_recv(sock, &ctx->remote_qp_attrs[i], S_QPA, 0) > 1) {
            counter.insert(i);
            count++;
		}
        else{
		    fprintf(stderr, "ERROR reading qp attrs from socket");
        }        
		printf("Server %d <-- Node %d's qp_attr: ", ctx->id, i);
		print_qp_attr(ctx->remote_qp_attrs[i]);
		
		// if(connect_ctx(ctx, ctx->local_qp_attrs[i].psn, 
		// 	ctx->remote_qp_attrs[i], ctx->qp[i], 1)) {  // Unreliable Connection
		// 	fprintf(stderr, "Couldn't connect to remote QP\n");
		// 	exit(0);
		// }
        i++;
        if (counter.size() == NODES_CNT - 1)
        {
            break;
        }
    }
    // cout << "Recvd QPs" << endl;
}

void send_thread(int id, struct context *ctx)
{
    // cout << "In Send" << endl;
    int i = 0, count = 0;
    set<int> counter;
    while (count <= NODES_CNT - 1)
    {
        if (i == id || counter.count(i) > 0)
        {
            i++;
            continue;
        }
        if (i > NODES_CNT - 1)
        {
            i = 0;
            continue;
        }
        int sock = send_sockets.find(i)->second;
        int s = nn_send(sock, &resp_area_stag[id], sizeof(struct stag) + 1, 1);
        /*if(send < 0)
        {
			fprintf(stderr, "send failed: %s\n", strerror(errno));
        }
        else*/
        if (s > 0)
        {
            // cout << "Sent to " << i << endl;
            counter.insert(i);
            count++;
        }
        
        i++;
        if (counter.size() == NODES_CNT - 1)
        {
            break;
        }
    }
    // cout << "Resp area sent" << endl;
    count = 0;
    i = 0;
    counter.clear();
    while (count <= NODES_CNT - 1)
    {
        if (i == id || counter.count(i) > 0)
        {
            i++;
            continue;
        }
        if (i > NODES_CNT - 1)
        {
            i = 0;
            continue;
        }
        int sock = send_sockets.find(i)->second;
        int s = nn_send(sock, &req_area_stag[id], sizeof(struct stag) + 1, 1);
        if (s > 1)
        {
            // cout << "Sent to" << i << endl;
            counter.insert(i);
            count++;
        }
        i++;
        if (counter.size() == NODES_CNT - 1)
        {
            break;
        }
    }
    count = 0;
    i = 0;
    counter.clear();
    while (count <= NODES_CNT - 1)
    {
        if (i == id || counter.count(i) > 0)
        {
            i++;
            continue;
        }
        if (i > NODES_CNT - 1)
        {
            i = 0;
            continue;
        }
        int sock = send_sockets.find(i)->second;
        int s = nn_send(sock, &ctx->local_qp_attrs[i], S_QPA, 1);
        if (s > 1)
        {
            counter.insert(i);
            count++;
        }
        i++;
        if (counter.size() == NODES_CNT - 1)
        {
            break;
        }
    }
    // cout << "QPs Sent" << endl;
    // cout << "Done Send" << endl;
}

int node(int node_id, struct context *ctx)
{
    int id = node_id, i;
    char myurl[30], url[30];
    int to = 100;

    resp_area_stag[id].rkey = resp_area_mr->rkey;
    resp_area_stag[id].size = 256 * KB;
    resp_area_stag[id].id = ctx->id;

    req_area_stag[id].id = ctx->id;
    req_area_stag[id].rkey = req_area_mr->rkey;
    req_area_stag[id].size = 256 * KB;

    // char my_serialized_stag[sizeof(struct stag)+1];
    // serialize(node_stags[id], my_serialized_stag);

    for (i = 0; i < NODES_CNT; i++)
    {
        if (i == id)
            continue;
        int temp_sock = nn_socket(AF_SP, NN_PAIR);
        uint64_t p = get_port_id(id, i);
        snprintf(url, 30, "tcp://%s:%ld", ifaddr[i], p);
        nn_connect(temp_sock, url);
        // cout << "Connected to :" << url << endl;
        assert(nn_setsockopt(temp_sock, NN_SOL_SOCKET, NN_SNDTIMEO, &to, sizeof(to)) >= 0);
        assert(nn_setsockopt(temp_sock, NN_SOL_SOCKET, NN_RCVTIMEO, &to, sizeof(to)) >= 0);
        send_sockets.insert(make_pair(i, temp_sock));
        // cout << send_sockets.size() <<endl;
    }

    for (i = 0; i < NODES_CNT; i++)
    {
        if (i == id)
            continue;
        int temp_sock = nn_socket(AF_SP, NN_PAIR);
        uint64_t p = get_port_id(i, id);
        snprintf(myurl, 30, "tcp://%s:%ld", ifaddr[id], p);
        nn_bind(temp_sock, myurl);
        // cout << "Binded to :" << myurl << endl;
        assert(nn_setsockopt(temp_sock, NN_SOL_SOCKET, NN_RCVTIMEO, &to, sizeof(to)) >= 0);
        assert(nn_setsockopt(temp_sock, NN_SOL_SOCKET, NN_SNDTIMEO, &to, sizeof(to)) >= 0);
        recv_sockets.insert(make_pair(i, temp_sock));
        // cout << recv_sockets.size() <<endl;
    }
    thread one(send_thread, id, ctx);
    thread two(recv_thread, id, ctx);

    one.join();
    two.join();
    cout << "resp_area stags:" << endl;

    for (int i = 0; i < NODES_CNT; i++)
    {
        if (i == id)
            continue;
        print_stag(resp_area_stag[i]);
    }
    cout << "req_area stags:" << endl;
    for (int i = 0; i < NODES_CNT; i++)
    {
        if (i == id)
            continue;
        print_stag(req_area_stag[i]);
    }
    for (int i = 0; i < NODES_CNT ; i++){
        if (i == id){
            continue;
        }
        nn_shutdown(recv_sockets[i], 0);
        nn_shutdown(send_sockets[i], 0);
    }
    return 0;
}

void rdma_send_thread(struct context *ctx, int total){
	*req_area = 121;
	int co = 0;
	while(co < total){
		for(int i = 0; i < NODES_CNT ;i++){
			if(i == ctx->id){
				continue;
			}
			int dest = i;
			rdma_send(ctx, dest, req_area, req_area_mr->lkey,0,64);
		}
		co++;
	}
}

void rdma_recv_thread(struct context *ctx, int total){
	int co = 0;
	while(co < total){
		for(int i = 0; i < NODES_CNT; i++){
			if(i == ctx->id){
				continue;
			}
			int src = i;
		 	rdma_recv(ctx, 1, src, resp_area, resp_area_mr->lkey,64);
			cout << "RDMA Recieved Data:" << *resp_area << endl;
		}
		co++;
	}
}

void singleThreadTest(struct context *ctx){
	if(ctx->id == 0){
		*req_area = 121;
		int dest = 1;
		rdma_send(ctx, dest, req_area, req_area_mr->lkey,0,64);
	}
	else{
		int src = 0;
		rdma_recv(ctx, 1, src, resp_area, resp_area_mr->lkey,64);
		cout << "RDMA Recieved Data:" << *resp_area << endl;
	}
}