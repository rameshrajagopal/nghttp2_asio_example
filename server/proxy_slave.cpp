#include <iostream>
#include <nghttp2/asio_http2_client.h>
#include <thread>
#include <syslog.h>
#include "queue.h"
#include "config.h"
#include "request_mapper.h"
#include <cassert>
using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

SlaveAddr slaveAddrArray[MAX_NUM_SLAVES] = {
    /* let the preprocessor and compiler do the actual concat */
    SLAVE_ADDR, SLAVE_PORT,  PROTOCOL SLAVE_ADDR SEMICOLON SLAVE_PORT WORKER_FILE, 
    SLAVE1_ADDR, SLAVE_PORT, PROTOCOL SLAVE1_ADDR SEMICOLON SLAVE_PORT WORKER_FILE,
    "127.0.0.1", "7000", "http://localhost:7000/work",
};

struct SlaveRequest
{
    SlaveRequest(int cnum, string m, string u, string req) :
        clientReqNum(cnum), method(m), uri(u), request(req) {}

    int clientReqNum;
    string method;
    string uri;
    string request;
};

#if 0
struct SlaveResponse
{
    SlaveResponse(int clientReqNum,  int responseLen)
    {
       cout << "SlaveResponse: " << clientReqNum << " " << responseLen << endl;
       clientReqNum = clientReqNum;
       len = responseLen;
       curPos = 0;
       //res = new uint8_t[responseLen];
    }
    void responseData(const uint8_t * data, uint32_t len)
    {
       //memcpy(&res[curPos], data, len);//FIXME
       curPos += len;
    }
    ~SlaveResponse() {
 //       delete res;
    }
    int clientReqNum;
    uint32_t len;
    uint32_t curPos;
//    uint8_t * res;
};
#endif

class ProxySlave {
public:
    ProxySlave(const string & address, const string & portnum, int num, 
               Queue<int> & rq): 
               addr(address), port(portnum), slaveNum(num), resQ(rq) 
    {}
    
    const string & getAddr() { return addr; }
    const string & getPort() { return port; }

    shared_ptr<SlaveRequest> getNextReq() 
    {
        return reqQ.pop();
    }
    void putReq(shared_ptr<SlaveRequest> sReq)
    {
        reqQ.push(sReq);
    }
    bool isRequestAvailable() 
    {
        return !reqQ.is_empty();
    }
    void putRes(int reqNum)
    {
        resQ.push(reqNum);
    }
private:
    int   slaveNum;
    const string & addr;
    const string & port;
    Queue<shared_ptr<SlaveRequest>> reqQ;
    Queue<int> & resQ;
};

extern int getRequestNum(shared_ptr<Stream> st);
extern void sendResponse(shared_ptr<Stream> st);
static void reqDispatcher(shared_ptr<ProxySlave> slave, shared_ptr<SlaveRequest> slaveReq, 
                   vector<session> & sessions, int sNum);

void SlaveIOTask(int sNum, shared_ptr<ProxySlave> slave, vector<session> & sessions)
{
    boost::system::error_code ec;
    boost::asio::io_service ios;

    sessions.push_back(session(ios, slave->getAddr(), slave->getPort()));
    sessions[sNum].on_connect([slave](tcp::resolver::iterator endpoint_it) {
       syslog(LOG_INFO, "connection established with %s\n", slave->getAddr().c_str());
    });
    sessions[sNum].on_error([slave](const boost::system::error_code &ec) {
       syslog(LOG_INFO, "connection error %s ec: %d\n", slave->getAddr().c_str(), ec.value());
    });
    ios.post([&sessions, sNum, slave]() {
        if (slave->isRequestAvailable()) {
           auto req = slave->getNextReq();
           reqDispatcher(slave, req, sessions, sNum);
        }
    });
    ios.run();
}

void ResRouterTask(RequestMap & reqMap, Queue<int> & resQ)
{
    while (true) {
        //SlaveResponse sr = resQ.pop();
        int clientReqNum = resQ.pop();
        int cnt = reqMap.decrementCnt(clientReqNum);
        syslog(LOG_INFO, "ResRouterTask got response %d %d\n", clientReqNum, cnt);
        if (cnt == 0) {
            syslog(LOG_INFO, "sending response to client reqNum: %d\n", clientReqNum);
            sendResponse(reqMap.getStream(clientReqNum));
        }
    }
}

void reqDispatcher(shared_ptr<ProxySlave> slave, shared_ptr<SlaveRequest> slaveReq, 
                   vector<session> & sessions, int sNum)
{
    header_map h;
    char buf[16] = {0};
    /* wrap the client request into header */
    snprintf(buf, sizeof(buf), "%d", slaveReq->clientReqNum);
    struct header_value hv = {buf, true};
    h.insert(make_pair("reqnum", hv));
    /* actual call to slave */
    syslog(LOG_INFO, "Sending request to slave: %s\n", slave->getAddr().c_str());
    boost::system::error_code ec;
    auto req = sessions[sNum].submit(ec, slaveReq->method, slaveReq->uri, h);
//    sessions[sNum].io_service().post([req, slave]() {
    req->on_response([slave](const response & res) {
            auto search = res.header().find("reqnum");
            assert(search != res.header().end());
            int  reqNum = std::stoi(search->second.value, nullptr, 10);

            auto kv = res.header().find("size");
            assert(kv != res.header().end());
            int expectedSize = std::stoi(kv->second.value, nullptr, 10);
            syslog(LOG_INFO, "Response expected size: %d %d\n", reqNum, expectedSize);
            //SlaveResponse sRes(reqNum, expectedSize);
            //cout << "sRes size: " << sRes.len << endl;
            res.on_data([slave, reqNum](const uint8_t * data, size_t len) {
                if (len == 0) {
                   //SlaveResponse sRes(reqNum, 0);
                   slave->putRes(reqNum);
                }
            });
     });
  //  });
}

void ReqRouterTask(Queue<shared_ptr<Stream>> & q, int numSlaves)
{
    vector<shared_ptr<ProxySlave>> slaves;
    vector<session> sessions;
    RequestMap reqMap;
//    Queue<SlaveResponse> resQ;
    Queue<int> resQ;

    syslog(LOG_INFO, "started proxy slaves: %d\n", numSlaves);
    if (numSlaves > MAX_NUM_SLAVES) {
        numSlaves = MAX_NUM_SLAVES;
    }
    for (int num = 0; num < numSlaves; ++num) {
        auto slave = make_shared<ProxySlave> (
                             slaveAddrArray[num].addr, slaveAddrArray[num].port, num, resQ);
        slaves.push_back(slave);
    }
    /* create Response Collector task */
    auto th = std::thread([&reqMap, &resQ]() {
        ResRouterTask(reqMap, resQ);
    });
    th.detach();
    /* create N slave IO tasks */
    for (int num = 0; num < numSlaves; ++num) {
        auto th = std::thread([&slaves, num, &sessions]() {
            SlaveIOTask(num, slaves[num], sessions);
        });
        th.detach();
    }
    while (true) {
        // pop the st from queue, if possible put the reqNum as part of stream
        auto st = q.pop();
        // decide to whom u need to send the route the requests 
        struct RequestIdentity identity = {numSlaves, 0, st};
        // using st, get the reqNum
        // using the referenceNum store the st into requestMap
        int clientReqNum = getRequestNum(st);
        reqMap.put(clientReqNum, identity);

        for (int num = 0; num < numSlaves; ++num) {
            shared_ptr<SlaveRequest>  sReq = make_shared<SlaveRequest> (clientReqNum, "GET", slaveAddrArray[num].uri, "ActualRequest");
            slaves[num]->putReq(sReq);
        }
    }
}

