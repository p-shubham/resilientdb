#include "global.h"
#include "nn.hpp"
#include <nanomsg/bus.h>
#include <nanomsg/pair.h>
#include "query.h"

#include <../deps/infinity/core/Context.h>
#include <infinity/queues/QueuePairFactory.h>
#include <infinity/queues/QueuePair.h>
#include <infinity/memory/Buffer.h>
#include <infinity/memory/RegionToken.h>
#include <infinity/requests/RequestToken.h>

class Workload;
class Message;


#define GET_RCV_NODE_ID(b) ((uint32_t *)b)[0]

class Transport_rdma{
    public:
        void init();
        string get_path(); 
        void read_ifconfig(const char *ifaddr_file);
        std::pair<infinity::core::Context *, infinity::queues::QueuePair *> setup_rdma_connection(uint64_t dest_node_id, uint64_t port, bool SENDER);
        int rdma_send(infinity::memory::Buffer *buf, infinity::queues::QueuePair *qp,infinity::core::Context *context);
        infinity::memory::Buffer* rdma_recv(infinity::core::Context *context);
        bool rdma_write(infinity::memory::Buffer *buf ,uint64_t dest_node_id, uint64_t thread_id);
        infinity::memory::Buffer * rdma_read(uint64_t dest_node_id, uint64_t read_thread_id);
        void disconnect();
    private:
        char **ifaddr;
        uint64_t _node_cnt;
	    uint64_t _sock_cnt;

        //All the IP, Port combinations are here
        std::vector <std::pair<uint64_t,uint64_t>> IP_Ports;
        /*
        *Following maps store the context,queue pair which are utilised for:
        *RDMA_Send, RDMA_recv and RDMA_write operations
        *they store: (thread_id,dest_node_id),(context,qp)
        */
        std::map<std::pair<uint64_t, uint64_t>, std::pair<infinity::core::Context *, infinity::queues::QueuePair *>> context_qp;

};