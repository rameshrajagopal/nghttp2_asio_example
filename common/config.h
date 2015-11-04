#ifndef _NGHTTP_CONFIG_H_INCLUDED_
#define _NGHTTP_CONFIG_H_INCLUDED_

#define  MASTER_NODE_ADDR  "192.168.0.241"
#define  MASTER_NODE_PORT  "8000"
#define  SEMICOLON         ":"
#define  SLASH             "/"
#define  MASTER_PORT       MASTER_NODE_ADDR SEMICOLON MASTER_NODE_PORT SLASH
#define  PROTOCOL          "http://"
#define  MASTER_NODE_URI   PROTOCOL MASTER_PORT
#define  WORKER_FILE       "/work"
#define within(num) (int) ((float) num * random() / (RAND_MAX + 1.0))

#define MAX_NUM_SLAVES  (3)
typedef struct {
    std::string addr;
    std::string port;
    std::string uri;
}SlaveAddr;

#define SLAVE_ADDR  "192.168.0.241"
#define SLAVE_PORT  "7000"
#define SLAVE1_ADDR "192.168.0.12"

#endif /*_NGHTTP_CONFIG_H_INCLUDED_*/
