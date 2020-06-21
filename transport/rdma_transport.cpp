#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include <infinity/core/Context.h>
#include <infinity/queues/QueuePairFactory.h>
#include <infinity/queues/QueuePair.h>
#include <infinity/memory/Buffer.h>
#include <infinity/memory/RegionToken.h>
#include <infinity/requests/RequestToken.h>

#include "global.h"
#include "rdma_transport.h"
#include "nn.hpp"
#include "query.h"
#include "message.h"

string Transport_rdma::get_path(){
        string path;
    #if SHMEM_ENV
        path = "/dev/shm/";
    #else
        char *cpath;
        cpath = getenv("SCHEMA_PATH");
        if (cpath == NULL)
            path = "./";
        else
            path = string(cpath);
    #endif
        path += "ifconfig.txt";
        return path;
}

/*
*For every send_thread in the replica,
*
*/
uint64_t get_port_id(uint64_t src_node_id, uint64_t dest_node_id, uint64_t send_thread_id){
    uint64_t port_id = 0;
    //DEBUG("Calc port id %ld %ld %ld\n", src_node_id, dest_node_id, send_thread_id);
    port_id += g_total_node_cnt * dest_node_id;
    //DEBUG("%ld\n", port_id);
    port_id += src_node_id;
    //DEBUG("%ld\n", port_id);
    //  uint64_t max_send_thread_cnt = g_send_thread_cnt > g_client_send_thread_cnt ? g_send_thread_cnt : g_client_send_thread_cnt;
    //  port_id *= max_send_thread_cnt;
    port_id += send_thread_id * g_total_node_cnt * g_total_node_cnt;
    //DEBUG("%ld\n", port_id);
    port_id += TPORT_PORT;
    //DEBUG("%ld\n", port_id);
    printf("Port ID:  %ld, %ld -> %ld : %ld\n", send_thread_id, src_node_id, dest_node_id, port_id);
    return port_id;
}
/*
*Reads if_config file in the passed address
*/
void Transport_rdma::read_ifconfig(const char *ifaddr_file)
{
    ifaddr = new char *[g_total_node_cnt];

    uint64_t cnt = 0;
    printf("Reading ifconfig file: %s\n", ifaddr_file);
    ifstream fin(ifaddr_file);
    string line;
    while (getline(fin, line))
    {
        ifaddr[cnt] = new char[line.length() + 1];
        strcpy(ifaddr[cnt], &line[0]);
        printf("%ld: %s\n", cnt, ifaddr[cnt]);
        cnt++;
    }
    assert(cnt == g_total_node_cnt);
}

/*This function creates the very first connection between the current node and the destination node
*@params: dest_node_id:  destination node id (uint64_t)
*@params: port: port number (uint64_t)
*@params: SENDER: Whether the node is going to send or recieve the first connection (Boolean)
*/
std::pair<infinity::core::Context *, infinity::queues::QueuePair *> Transport_rdma::setup_rdma_connection(uint64_t dest_node_id, uint64_t port, bool SENDER){
    infinity::core::Context *context = new infinity::core::Context();
    infinity::queues::QueuePairFactory *qpFactory = new infinity::queues::QueuePairFactory(context);
	infinity::queues::QueuePair *qp;
    if(SENDER){
        char* IP = ifaddr[dest_node_id];
        qp = qpFactory->connectToRemoteHost(IP, port);
        printf("Sending first RDMA message to %ld \n", dest_node_id);
        infinity::memory::Buffer *sendBuffer = new infinity::memory::Buffer(context, 128 * sizeof(char));
        infinity::memory::Buffer *receiveBuffer = new infinity::memory::Buffer(context, 128 * sizeof(char));
        context->postReceiveBuffer(receiveBuffer);
        qp->send(sendBuffer, 128 * sizeof(char), context->defaultRequestToken);
        context->defaultRequestToken->waitUntilCompleted();
        printf("Sent first RDMA message\n");
        return make_pair(context,qp);
    }
    else{
        infinity::memory::Buffer *bufferToReadWrite = new infinity::memory::Buffer(context, 128 * sizeof(char));
		infinity::memory::RegionToken *bufferToken = bufferToReadWrite->createRegionToken();
        printf("Setting up connection (blocking)\n");
		qpFactory->bindToPort(port);
		qp = qpFactory->acceptIncomingConnection(bufferToken, sizeof(infinity::memory::RegionToken));
        printf("Recieved first message from %ld \n", dest_node_id);
        return make_pair(context,qp);
    }
}

/*
*This Function sends accross a buffer to another node
*@params: buf: buffer to be sent
*@params: qp: Queue Pair of the sender
*@params: Context: Context of the sender to be used to send the message
*@returns: 1 if completed
*/
int Transport_rdma::rdma_send(infinity::memory::Buffer *buf,uint64_t dest_node_id, uint64_t thread_id){
    std::pair<infinity::core::Context *, infinity::queues::QueuePair *> pair = send_pairs.find(make_pair(dest_node_id,thread_id))->second;
    infinity::core::Context *context = pair.first;
    infinity::queues::QueuePair *qp = pair.second;
    uint64_t size = buf->getSizeInBytes();
    printf("Sending the message\n");
    infinity::requests::RequestToken requestToken(context);
    qp->send(buf, size , &requestToken);
    return 1;
}
/*
*TO DO: NOT COMPLETE, WONT WORK
*This function recieves the message on the current node
*@params: context: the context of the sender node
*@params: qp: QP
*@returns: Buffer: returns pointer the message recieved
*/
infinity::memory::Buffer* Transport_rdma::rdma_recv(uint64_t thread_id){
    infinity::core::Context *context;
    printf("Initializing recieve buffer\n");
    infinity::memory::Buffer *bufferToReceive = new infinity::memory::Buffer(context, ((size_t) -1));
	context->postReceiveBuffer(bufferToReceive);
    printf("Waiting for message (blocking)\n");
    infinity::core::receive_element_t receiveElement;
	while (!context->receive(&receiveElement));
	context->postReceiveBuffer(receiveElement.buffer);
    return bufferToReceive;
}
/*
*@function: rdma_write()
*@params: remote_qp = qpFactory->connectToRemoteHost(IP,port)
*@params: context
*@params: buf: Write buffer
*@return: bool when done
*/
bool Transport_rdma::rdma_write(void *buf ,uint64_t dest_node_id, uint64_t thread_id){
    std::pair<infinity::core::Context *, infinity::queues::QueuePair *> pair = send_pairs.find(make_pair(dest_node_id,thread_id))->second;
    infinity::core::Context *context = pair.first;
    infinity::queues::QueuePair *remote_qp = pair.second;
    infinity::requests::RequestToken request_token(context);
    infinity::memory::RegionToken *rmt_buf_token = (infinity::memory::RegionToken *) remote_qp->getUserData();
    remote_qp->write((infinity::memory::Buffer *) buf, rmt_buf_token, &request_token);
    request_token.waitUntilCompleted();
    return true;
}
/*
*@function: rdma_read()
*@params: remote_qp = qpFactory->connectToRemoteHost(IP,port)
*@params: context
*@return: buffer when done
*/
infinity::memory::Buffer* Transport_rdma::rdma_read(uint64_t dest_node_id, uint64_t read_thread_id){
    std::pair<infinity::core::Context *, infinity::queues::QueuePair *> pair = send_pairs.find(make_pair(dest_node_id,read_thread_id))->second;
    infinity::core::Context *context = pair.first;
    infinity::queues::QueuePair *remote_qp = pair.second;
    infinity::requests::RequestToken request_token(context);
    infinity::memory::RegionToken *rmt_buf_token = (infinity::memory::RegionToken *) remote_qp->getUserData();
    infinity::memory::Buffer *read_buf = new infinity::memory::Buffer(context, 128 * sizeof(char));
    remote_qp->read(read_buf, rmt_buf_token, &request_token);
    request_token.waitUntilCompleted();
    return read_buf;
}
/*
*Following function initailizes the transport manager in the replica or client
*/
void Transport_rdma::init(){

    string path = get_path();
    read_ifconfig(path.c_str());

    printf("Initializing RDMA Transport manager for node %d", g_node_id);

    for(uint64_t node_id = 0; node_id < g_total_node_cnt; node_id++){
        
        if(node_id == g_node_id){
            continue;
        }
        //Configuring the recieving contexts and Queue Pairs
        if(ISCLIENTN(node_id)){
            for (uint64_t client_thread_id = g_client_thread_cnt + g_client_rem_thread_cnt; client_thread_id < g_client_thread_cnt + g_client_rem_thread_cnt + g_client_send_thread_cnt; client_thread_id++)
            {
                uint64_t port_id = get_port_id(node_id, g_node_id, client_thread_id % g_client_send_thread_cnt);
                if (!ISSERVER)
                {
                    recv_.push_back(setup_rdma_connection(node_id,port_id,false));
                }
                else
                {
                    recv_clients.push_back(setup_rdma_connection(node_id,port_id,false));
                }
                //DEBUG("Socket insert: {%ld}: %ld\n", node_id, (uint64_t)sock);
            }
        }
        else{
            for (uint64_t server_thread_id = g_thread_cnt + g_rem_thread_cnt; server_thread_id < g_thread_cnt + g_rem_thread_cnt + g_send_thread_cnt; server_thread_id++)
            {
                uint64_t port_id = get_port_id(node_id, g_node_id, server_thread_id % g_send_thread_cnt);
                if (!ISSERVER)
                {
                    recv_.push_back(setup_rdma_connection(node_id,port_id,false));
                }
                else
                {
                    if (node_id % (g_this_rem_thread_cnt - 1) == 0)
                    {
                        recv_replicas_1.push_back(setup_rdma_connection(node_id,port_id,false));
                    }
                    else
                    {
                        recv_replicas_2.push_back(setup_rdma_connection(node_id,port_id,false));
                    }
                }
                //DEBUG("Socket insert: {%ld}: %ld\n", node_id, (uint64_t)sock);
            }
        }
        //Configuring Send Maps
        if (ISCLIENTN(g_node_id))
        {
            for (uint64_t client_thread_id = g_client_thread_cnt + g_client_rem_thread_cnt; client_thread_id < g_client_thread_cnt + g_client_rem_thread_cnt + g_client_send_thread_cnt; client_thread_id++)
            {
                uint64_t port_id = get_port_id(g_node_id, node_id, client_thread_id % g_client_send_thread_cnt);
                std::pair<uint64_t, uint64_t> sender = std::make_pair(node_id, client_thread_id);
                std::pair<infinity::core::Context *, infinity::queues::QueuePair *> temp = setup_rdma_connection(node_id,port_id,true);
                send_pairs.insert(std::make_pair(sender, temp));
                //DEBUG("Socket insert: {%ld,%ld}: %ld\n", node_id, client_thread_id, (uint64_t)sock);
            }
        }
        else
        {
            for (uint64_t server_thread_id = g_thread_cnt + g_rem_thread_cnt; server_thread_id < g_thread_cnt + g_rem_thread_cnt + g_send_thread_cnt; server_thread_id++)
            {
                uint64_t port_id = get_port_id(g_node_id, node_id, server_thread_id % g_send_thread_cnt);
                std::pair<uint64_t, uint64_t> sender = std::make_pair(node_id, server_thread_id);
                std::pair<infinity::core::Context *, infinity::queues::QueuePair *> temp = setup_rdma_connection(node_id,port_id,true);
                send_pairs.insert(std::make_pair(sender, temp));
                //DEBUG("Socket insert: {%ld,%ld}: %ld\n", node_id, server_thread_id, (uint64_t)sock);
            }
        }
    }
}