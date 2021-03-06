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
        reqQ = make_shared<Queue<int>>();
    }
    
    const string & getAddr() { return addr; }
    const string & getPort() { return port; }

    int getTaskNum(void) { return ioTaskNum; }
    void setTaskNum(int num) { ioTaskNum = num; }

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
    int   ioTaskNum;
    const string & addr;
    const string & port;
    shared_ptr<Queue<int>>  reqQ;
    Queue<int> & resQ;
    bool status;
};

extern int getRequestNum(shared_ptr<Stream> st);
extern void sendResponse(shared_ptr<Stream> st, bool randomness, int bytes, bool compression);
static void reqDispatcher(shared_ptr<ProxySlave> slave, int clientReqNum, 
                   vector<session> & sessions, int sNum);

class SlaveIOTask {
public:
    SlaveIOTask(std::mutex & m, int sNum): vMutex(m), taskNum(sNum) {}

    void setTaskNum(int sNum) {
        taskNum = sNum;
    }
    void run(shared_ptr<ProxySlave> slave, vector<session> & sessions)
    {
            boost::system::error_code ec;
            int sNum = 0;
            auto connected = false;

            std::unique_lock<std::mutex> mlock(vMutex);
            sessions.push_back(session(ios, slave->getAddr(), slave->getPort()));
            sNum = sessions.size()-1;
            mlock.unlock();
            this->setTaskNum(sNum);
            sessions[sNum].on_connect([slave, &connected](tcp::resolver::iterator endpoint_it) {
                    SYSLOG(LOG_INFO, "connection established with %s\n", slave->getAddr().c_str());
                    connected = true;
                    slave->setStatus(connected);
                    });
            sessions[sNum].on_error([slave, &connected](const boost::system::error_code &ec) {
                    cout << "connection error with " << slave->getAddr() << " " << ec.value() << endl;
                    SYSLOG(LOG_INFO, "connection error %s ec: %d\n", slave->getAddr().c_str(), ec.value());
                    connected = false;
                    slave->setStatus(connected);
                    });
            ios.run();
            if (connected) {
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
    }
    void post(shared_ptr<ProxySlave> slave, 
              vector<session> & sessions, int clientReqNum)
    {
            ios.post([&sessions, slave, clientReqNum, this]() {
                 reqDispatcher(slave, clientReqNum, sessions, taskNum);
            });
    }
private: 
   boost::asio::io_service ios;
   std::mutex & vMutex;
   int taskNum;
};

void ResRouterTask(RequestMap & reqMap, Queue<int> & resQ, ConfigFile & cfg)
{
    while (true) {
        int clientReqNum = resQ.pop();
        int cnt = reqMap.decrementCnt(clientReqNum);
        SYSLOG(LOG_INFO, "ResRouterTask got response %d %d\n", clientReqNum, cnt);
        if (cnt == 0) {
            SYSLOG(LOG_INFO, "sending response to client reqNum: %d\n", clientReqNum);
            bool randomness = (cfg.getValueOfKey<string>("randomness") == "yes") ? true : false;
            int  reply_bytes = cfg.getValueOfKey<int>("reply_bytes");
            bool compression = (cfg.getValueOfKey<string>("compression") == "yes") ? true : false;
            sendResponse(reqMap.getStream(clientReqNum), randomness, reply_bytes, compression);
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
    SYSLOG(LOG_INFO, "Sending request to slave: %s\n", slave->getAddr().c_str());
    boost::system::error_code ec;
    slave->slaveReqMap[clientReqNum] = clientReqNum;
    auto request_generator = [](uint8_t * buf, size_t len, uint32_t * flags) -> ssize_t {
        memset(buf, 'd', REQUEST_SIZE);
        *flags = NGHTTP2_DATA_FLAG_EOF;
        return REQUEST_SIZE;
    }; 
    char data[1024];
    memset(data, 'c', sizeof(data));
    auto req = sessions[sNum].submit(ec, "POST", slaveAddrArray[sNum].uri, data, h);
    req->on_response([slave](const response & res) {
            auto search = res.header().find("reqnum");
            assert(search != res.header().end());
            int  reqNum = std::stoi(search->second.value, nullptr, 10);

            auto kv = res.header().find("size");
            assert(kv != res.header().end());
            int expectedSize = std::stoi(kv->second.value, nullptr, 10);
            SYSLOG(LOG_INFO, "Response expected size: %d %d\n", reqNum, expectedSize);
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
          SYSLOG(LOG_INFO, "req close event: %d\n", clientReqNum);
          slave->slaveReqMap.erase(clientReqNum);
     });
}

void ReqRouterTask(Queue<shared_ptr<Stream>> & q, ConfigFile & cfg)
{
    vector<shared_ptr<ProxySlave>> slaves;
    vector<session> sessions;
    RequestMap reqMap;
    vector<shared_ptr<SlaveIOTask>> iotasks;
    Queue<int> resQ;

    int numSlaves = cfg.getValueOfKey<int>("num_slaves", 0);
    string slavePort = cfg.getValueOfKey<string>("slave_port");
    for (int num = 0; num < numSlaves; ++num) {
        string addr = cfg.getValueOfKey<string>("slave_addr" + to_string(num));
        slaveAddrArray[num].addr = addr;
        slaveAddrArray[num].port = slavePort;
        slaveAddrArray[num].uri  = "http://" + addr + ":" + slavePort + "/work";
    }
    cout << "started proxy slaves: " <<  numSlaves << endl;
    SYSLOG(LOG_INFO, "started proxy slaves: %d\n", numSlaves);
    for (int num = 0; num < numSlaves; ++num) {
        auto slave = make_shared<ProxySlave> (
                             slaveAddrArray[num].addr, slaveAddrArray[num].port, num, resQ);
        slaves.push_back(slave);
    }
    /* create Response Collector task */
    auto th = std::thread([&reqMap, &resQ, &cfg]() {
        ResRouterTask(reqMap, resQ, cfg);
    });
    th.detach();
    /* create N slave IO tasks */
    std::mutex vMutex;
    for (int num = 0; num < numSlaves; ++num) {
        auto th = std::thread([&slaves, num, &sessions, &iotasks, &vMutex]() {
            std::unique_lock<std::mutex> mlock(vMutex);
            iotasks.push_back(make_shared<SlaveIOTask> (vMutex, num));
            int tasknum = iotasks.size()-1;
            slaves[num]->setTaskNum(tasknum);
            mlock.unlock();
            iotasks[tasknum]->run(slaves[num], sessions);
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
                iotasks[slaves[num]->getTaskNum()]->post(slaves[num], sessions, clientReqNum);
                ++numReplies;
            }
        }
        if (numReplies > 0) {
            identity.cnt = numReplies;
            reqMap.put(clientReqNum, identity);
        } else {
            bool randomness = (cfg.getValueOfKey<string>("randomness") == "yes") ? true : false;
            int  reply_bytes = cfg.getValueOfKey<int>("reply_bytes");
            bool compression = (cfg.getValueOfKey<string>("compression") == "yes") ? true : false;
            sendResponse(st, randomness, reply_bytes, compression);
        }
    }
}

