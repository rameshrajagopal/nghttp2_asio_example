#include <iostream>
#include <nghttp2/asio_http2_client.h>
#include <thread>
#include <syslog.h>
#include "queue.h"
#include "config.h"
using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

SlaveAddr slaveAddrArray[MAX_NUM_SLAVES] = {
    SLAVE_ADDR, SLAVE_PORT, "http://192.168.0.241:7000/work",
    "192.168.0.203", "7000", "http://192.168.0.203:7000/work",
    "127.0.0.1", "7000", "http://localhost:7000/work",
};

class ProxySlave {
public:
    ProxySlave(const string & address, const string & portnum, int num): 
        addr(address), port(portnum), slaveNum(num) {}
    
    const string & getAddr() { return addr; }
    const string & getPort() { return port; }

    void send(vector<session> & sessions, const string & method, const string & uri, 
            response_cb response, header_map h={}) {
        boost::system::error_code ec;
        auto req = sessions[slaveNum].submit(ec, method, uri, h);
        req->on_response(response);
        req->on_close([](uint32_t status) {
            cout << "request closed" << endl;
        });
    }
private:
    int   slaveNum;
    const string & addr;
    const string & port;
};

class Stream;//forward
struct requestIdentity {
    int cnt;
    int expectedSize;
    shared_ptr<Stream> stream;
};

using requestMap = std::map<int, requestIdentity>;
extern int getRequestNum(shared_ptr<Stream> st);
extern void sendResponse(shared_ptr<Stream> st);

void SlaveTask(Queue<shared_ptr<Stream>> & q, int numSlaves)
{
    vector<session> sessions;
    vector<shared_ptr<ProxySlave>> slaves;
    requestMap reqMap;

    syslog(LOG_INFO, "started proxy slavesi: %d\n", numSlaves);
    if (numSlaves > MAX_NUM_SLAVES) {
        numSlaves = MAX_NUM_SLAVES;
    }
    for (int num = 0; num < numSlaves; ++num) {
        auto slave = make_shared<ProxySlave> (slaveAddrArray[num].addr, slaveAddrArray[num].port, num);
        slaves.push_back(slave);
        auto th = std::thread([num, slave, &sessions]() {
                boost::system::error_code ec;
                boost::asio::io_service io_service;

                sessions.push_back(session(io_service, slave->getAddr(), slave->getPort()));
                sessions[num].on_connect([num](tcp::resolver::iterator endpoint_it) {
                        syslog(LOG_INFO, "connection established: %d\n", num);
                        });
                sessions[num].on_error([num](const boost::system::error_code &ec) {
                        syslog(LOG_INFO, "connection error: %d\n", ec.value());
                        });
                io_service.run();
                });
        th.detach();
    }
    int clientReqNum = 0;
    while (true) {
        char buf[16];
        // pop the st from queue, if possible put the reqNum as part of stream
        auto st = q.pop();
        struct requestIdentity identity = {numSlaves, 0, st};
        // using st, get the reqNum
        // using the referenceNum store the st into requestMap
        clientReqNum = getRequestNum(st);
        reqMap[clientReqNum] = identity;
        //send request to multiple slaves 
        // on data callback, take the reqNum and get the stream entity
        for (int num = 0; num < numSlaves; ++num) {
            boost::system::error_code ec;
            header_map h;
            /* put the reqNum into the header */
            snprintf(buf, sizeof(buf), "%d", clientReqNum);
            struct header_value hv = {buf, true};
            h.insert(make_pair("reqnum", hv));
            /* make a request to slave */
            syslog(LOG_INFO, "Sending request to slave:%d\n", num);
            auto req = sessions[num].submit(ec, "GET", slaveAddrArray[num].uri, h); 
            req->on_response([&reqMap](const response & res) {
#if 0
                for (auto &kv : res.header()) {
                   cout << kv.first << ":" << kv.second.value << endl;
                }
#endif
                    auto search = res.header().find("reqnum");
                    int reqNum = 0;
                    if (search != res.header().end()) {
                       reqNum = std::stoi(search->second.value, nullptr, 10);
                       cout << reqNum << endl;  
                    }
                    auto kv = res.header().find("size");
                    if (kv != res.header().end()) {
                       reqMap[reqNum].expectedSize = std::stoi(kv->second.value, nullptr, 10);
                    }
                    res.on_data([&reqMap, reqNum](const uint8_t * data, size_t len) {
                        syslog(LOG_INFO, "got response: %d len: %ld\n", reqMap[reqNum].cnt, len);
                        reqMap[reqNum].expectedSize -= len;
                        if (reqMap[reqNum].expectedSize == 0) {
                           --reqMap[reqNum].cnt;
                        }
                        if (reqMap[reqNum].cnt == 0) {
                           syslog(LOG_INFO, "sending response back to client\n");
                           sendResponse(reqMap[reqNum].stream);
                        }
                    });
            });
            req->on_close([](uint32_t status) {
                    syslog(LOG_INFO, "request got closed\n");
                    });
            }
     }
}
