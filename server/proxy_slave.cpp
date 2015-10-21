#include <iostream>
#include <nghttp2/asio_http2_client.h>
#include <thread>
#include "queue.h"
using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

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

#define MAX_NUM_SLAVES  (1)
typedef struct {
    string addr;
    string port;
    string uri;
}SlaveAddr;
SlaveAddr slaveAddrArray[MAX_NUM_SLAVES] = {
    "localhost", "7000", "http://localhost:7000/work",
//    "localhost", "7000", "http://localhost:7000/work",
};

class Stream;//forward
struct requestIdentity {
    int cnt;
    shared_ptr<Stream> stream;
};

using requestMap = std::map<int, requestIdentity>;
extern int getRequestNum(shared_ptr<Stream> st);
extern void sendResponse(shared_ptr<Stream> st);

void SlaveTask(Queue<shared_ptr<Stream>> & q)
{
    vector<session> sessions;
    vector<shared_ptr<ProxySlave>> slaves;
    requestMap reqMap;

    for (int num = 0; num < MAX_NUM_SLAVES; ++num) {
        auto slave = make_shared<ProxySlave> (slaveAddrArray[num].addr, slaveAddrArray[num].port, num);
        slaves.push_back(slave);
        auto th = std::thread([num, slave, &sessions]() {
                boost::system::error_code ec;
                boost::asio::io_service io_service;

#if 1
                sessions.push_back(session(io_service, slave->getAddr(), slave->getPort()));
                sessions[num].on_connect([num](tcp::resolver::iterator endpoint_it) {
                        cout << "connection established:  " << num << endl;
                        });
                sessions[num].on_error([num](const boost::system::error_code &ec) {
                        cout << "connection error: " << ec.message() << std::endl;
                        });
#else
                session sess(io_service, slave->getAddr(), slave->getPort());
                sess.on_connect([](tcp::resolver::iterator endpoint_it) {
                    cout << "connection established" << endl;
                    });
                sess.on_error([](const boost::system::error_code & ec) {
                    cout << "connection error " << ec << endl;
                    });
#endif
                io_service.run();
                });
        th.detach();
    }
    int clientReqNum = 0;
    while (true) {
        header_map h;
        char buf[16];
        // pop the st from queue, if possible put the reqNum as part of stream
        auto st = q.pop();
        struct requestIdentity identity = {1, st};
        // using st, get the reqNum
        // using the referenceNum store the st into requestMap
        clientReqNum = getRequestNum(st);
        reqMap[clientReqNum] = identity;
        //send request to multiple slaves 
        // on data callback, take the reqNum and get the stream entity
        for (int num = 0; num < MAX_NUM_SLAVES; ++num) {
            boost::system::error_code ec;
            /* put the reqNum into the header */
            snprintf(buf, sizeof(buf), "%d", clientReqNum);
            struct header_value hv = {buf, true};
            h.insert(make_pair("reqnum", hv));
            /* make a request to slave */
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
                       --reqMap[reqNum].cnt;
                       res.on_data([&reqMap, reqNum](const uint8_t * data, size_t len) {
                        cout << "Response: " <<  reqMap[reqNum].cnt << endl;
                        if (reqMap[reqNum].cnt == 0) {
                            sendResponse(reqMap[reqNum].stream);
                        }
                        });
                    }
            });
            req->on_close([](uint32_t status) {
                    cout << "request closed" << endl;
                    });
            }
     }
}
