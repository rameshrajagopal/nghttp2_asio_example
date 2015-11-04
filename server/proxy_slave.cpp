#include <iostream>
#include <fstream>
#include <sstream>
#include <nghttp2/asio_http2_client.h>
#include <thread>
#include <syslog.h>
#include "queue.h"
#include "config.h"
#include "request_mapper.h"
#include <cassert>
using boost::asio::ip::tcp;

#define REQUEST_SIZE  (1024)

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

#define MAX_PRE_DEFINED_SLAVES (20)
SlaveAddr slaveAddrArray[MAX_PRE_DEFINED_SLAVES];

struct SlaveRequest
{
    SlaveRequest(int cnum, string m, string u, string req) :
        clientReqNum(cnum), method(m), uri(u), request(req) {}

    int clientReqNum;
    string method;
    string uri;
    string request;
};

class ProxySlave {
public:
    ProxySlave(const string & address, const string & portnum, int num, 
               Queue<int> & rq): 
               addr(address), port(portnum), slaveNum(num), resQ(rq), status(false)
    {
        cout << address << " " << portnum << endl;
        reqQ = make_shared<Queue<int>>();
    }
    
    const string & getAddr() { return addr; }
    const string & getPort() { return port; }

    void setStatus(bool s) {
        status = s;
    }

    bool getStatus(void) {
        return status;
    }

    int getNextReq() 
    {
        return reqQ->pop();
    }
    void putReq(int reqNum)
    {
        reqQ->push(reqNum);
    }
    bool isRequestAvailable() 
    {
        return !reqQ->is_empty();
    }
    void putRes(int reqNum)
    {
        resQ.push(reqNum);
    }
    std::map<int, int> slaveReqMap;
private:
    int   slaveNum;
    const string & addr;
    const string & port;
//    Queue<shared_ptr<SlaveRequest>> reqQ;
    shared_ptr<Queue<int>>  reqQ;
    Queue<int> & resQ;
    bool status;
};

extern int getRequestNum(shared_ptr<Stream> st);
extern void sendResponse(shared_ptr<Stream> st);
static void reqDispatcher(shared_ptr<ProxySlave> slave, int clientReqNum, 
                   vector<session> & sessions, int sNum);

class SlaveIOTask {
public:
    SlaveIOTask() {}
    void run(shared_ptr<ProxySlave> slave, vector<session> & sessions, int sNum)
    {
            boost::system::error_code ec;

            sessions.push_back(session(ios, slave->getAddr(), slave->getPort()));
            sessions[sNum].on_connect([slave](tcp::resolver::iterator endpoint_it) {
                    syslog(LOG_INFO, "connection established with %s\n", slave->getAddr().c_str());
                    slave->setStatus(true);
                    });
            sessions[sNum].on_error([slave](const boost::system::error_code &ec) {
                    syslog(LOG_INFO, "connection error %s ec: %d\n", slave->getAddr().c_str(), ec.value());
                    slave->setStatus(false);
                    });
            ios.run();
            cout << "slave " << sNum << " went down " << endl;
            slave->setStatus(false);
            auto it = slave->slaveReqMap.begin();
            int pending_requests = 0;
            for (; it != slave->slaveReqMap.end(); ++it) {
                slave->putRes(it->first);
                ++pending_requests;
            }
            cout << "pending requests " << pending_requests << endl;
            slave->slaveReqMap.clear();
    }
    void post(shared_ptr<ProxySlave> slave, 
              vector<session> & sessions, int sNum, int clientReqNum)
    {
            ios.post([&sessions, sNum, slave, clientReqNum]() {
                 reqDispatcher(slave, clientReqNum, sessions, sNum);
            });
    }
private: 
   boost::asio::io_service ios;
};

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

void reqDispatcher(shared_ptr<ProxySlave> slave, int clientReqNum,
                   vector<session> & sessions, int sNum)
{
    header_map h;
    char buf[16] = {0};
    /* wrap the client request into header */
    snprintf(buf, sizeof(buf), "%d", clientReqNum);
    struct header_value hv = {buf, true};
    h.insert(make_pair("reqnum", hv));
    /* actual call to slave */
    syslog(LOG_INFO, "Sending request to slave: %s\n", slave->getAddr().c_str());
    boost::system::error_code ec;
    slave->slaveReqMap[clientReqNum] = clientReqNum;
    auto request_generator = [](uint8_t * buf, size_t len, uint32_t * flags) -> ssize_t {
        memset(buf, 'd', REQUEST_SIZE);
        *flags = NGHTTP2_DATA_FLAG_EOF;
        return REQUEST_SIZE;
    }; 
    auto req = sessions[sNum].submit(ec, "POST", slaveAddrArray[sNum].uri, 
                                    request_generator, h);
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
     req->on_close([slave, clientReqNum](uint32_t status){
          syslog(LOG_INFO, "req close event: %d\n", clientReqNum);
          slave->slaveReqMap.erase(clientReqNum);
     });
}

int parseConfigFile(const char * filename)
{
    ifstream is(filename);

    string line;
    int sNum = 0;
    while (std::getline(is, line)) {
        if (line[0] != '\n') {
            stringstream ss(line);
            ss >> slaveAddrArray[sNum].addr >> slaveAddrArray[sNum].port >> slaveAddrArray[sNum].uri;
            ++sNum;
        }
    }
    return sNum; 
}

void ReqRouterTask(Queue<shared_ptr<Stream>> & q, string configFile)
{
    vector<shared_ptr<ProxySlave>> slaves;
    vector<session> sessions;
    RequestMap reqMap;
    vector<shared_ptr<SlaveIOTask>> iotasks;
//    Queue<SlaveResponse> resQ;
    Queue<int> resQ;

    int numSlaves = parseConfigFile(configFile.c_str());

    cout << "started proxy slaves: " <<  numSlaves;
    syslog(LOG_INFO, "started proxy slaves: %d\n", numSlaves);
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
        auto th = std::thread([&slaves, num, &sessions, &iotasks]() {
            iotasks.push_back(make_shared<SlaveIOTask> ());
            iotasks[num]->run(slaves[num], sessions, num);
        });
        th.detach();
    }
    while (true) {
        // pop the st from queue, if possible put the reqNum as part of stream
        auto st = q.pop();
        int numReplies = 0;
        // decide to whom u need to send the route the requests 
        struct RequestIdentity identity = {numReplies, 0, st};
        // using st, get the reqNum
        int clientReqNum = getRequestNum(st);
        // using the referenceNum store the st into requestMap
        for (int num = 0; num < numSlaves; ++num) {
            if (slaves[num]->getStatus()) {
                iotasks[num]->post(slaves[num], sessions, num, clientReqNum);
                ++numReplies;
            }
        }
        if (numReplies > 0) {
            identity.cnt = numReplies;
            reqMap.put(clientReqNum, identity);
        } else {
            sendResponse(st);
        }
    }
}

