#ifndef _NGHTTP_CONFIG_H_INCLUDED_
#define _NGHTTP_CONFIG_H_INCLUDED_

#define  MASTER_NODE_ADDR  "127.0.0.1"
#define  MASTER_NODE_PORT  "8000"
#define  MASTER_NODE_URI   "http://127.0.0.1:8000/"

#define MAX_NUM_SLAVES  (3)
typedef struct {
    std::string addr;
    std::string port;
    std::string uri;
}SlaveAddr;

SlaveAddr slaveAddrArray[MAX_NUM_SLAVES] = {
    "127.0.0.1", "7000", "http://localhost:7000/work",
    "192.168.0.241", "7000", "http://192.168.0.241:7000/work",
    "192.168.0.203", "7000", "http://192.168.0.203:7000/work",
};

#endif /*_NGHTTP_CONFIG_H_INCLUDED_*/
