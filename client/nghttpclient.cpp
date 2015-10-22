#include <iostream>
#include <thread>
#include <nghttp2/asio_http2_client.h>
#include <config.h>
#include <syslog.h>

using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

#define MAX_NUM_CLIENTS  (100)
#define MAX_NUM_REQUESTS (1000)

void clientTask(const int clientNum, const int max_requests, const string master_addr, const string master_port)
{
    boost::system::error_code ec;
    boost::asio::io_service io_service;

    syslog(LOG_INFO, "client task: %d", clientNum);
    session sess(io_service, master_addr, master_port);
    map<string, int> clientMap;
    sess.on_connect([&sess, clientNum, max_requests, &clientMap](tcp::resolver::iterator endpoint_it) {
            boost::system::error_code ec;
            auto printer = [&clientMap](const response &res) {
                string key;
                auto search = res.header().find("clientreq");
                if (search != res.header().end()) {
                    key = search->second.value;
                }
                auto kv = res.header().find("size");
                if (kv != res.header().end()) {
                    int expectedSize = std::stoi(kv->second.value, nullptr, 10);
                    clientMap[key] = expectedSize;
                }
                res.on_data([&clientMap, key](const uint8_t * data, size_t len) {
                   clientMap[key] -= len;
                   syslog(LOG_INFO, "data from server of len: %d\n", len);
                   if (len == 0) {
                       struct timeval curtime;
                       gettimeofday(&curtime, NULL);
                       syslog(LOG_INFO, "response:%s sec:%ld usec: %ld\n", key.c_str(), curtime.tv_sec, curtime.tv_usec);
                   }
                });
            };
            std::size_t num = max_requests;
            auto count = make_shared<int>(num);
            char buf[16];

            struct timeval tstart, curtime;
            gettimeofday(&tstart, NULL);
            auto startPtr = make_shared<struct timeval>(tstart);

            for (size_t i = 0; i < num; ++i) {
            header_map h;
            snprintf(buf, sizeof(buf), "%d:%ld", clientNum, i);
            struct header_value hv = {buf, true};
            h.insert(make_pair("clientreq", hv));
            clientMap[buf] = 0;
            gettimeofday(&curtime, NULL);
            syslog(LOG_INFO, "request:%s sec: %ld usec:%ld\n", buf, curtime.tv_sec, curtime.tv_usec);
            auto req = sess.submit(ec, "GET", MASTER_NODE_URI, h);
            req->on_response(printer);
            req->on_close([&sess, count, startPtr, clientNum](uint32_t error_code) {
                if (--*count == 0) {
                    struct timeval tend, tdiff;
                    gettimeofday(&tend, NULL);
                    if (tend.tv_usec < startPtr->tv_usec) {
                         tdiff.tv_sec = tend.tv_sec - startPtr->tv_sec - 1;
                         tdiff.tv_usec = 1000000 + tend.tv_usec - startPtr->tv_usec;
                    } else {
                         tdiff.tv_sec = tend.tv_sec - startPtr->tv_sec;
                         tdiff.tv_usec = tend.tv_usec - startPtr->tv_usec;
                    }
                    int total_msec = (tdiff.tv_sec * 1000) + (tdiff.tv_usec/1000);
                    syslog(LOG_INFO, "client: %d total msec: %d\n", clientNum, total_msec);
                    sess.shutdown();
                }
                });
            }
    });

    sess.on_error([clientNum](const boost::system::error_code &ec) {
            syslog(LOG_INFO, "session connection error: %d\n", clientNum);
            });

    io_service.run();
}

int main(int argc, char *argv[])
{
    int max_threads, max_requests;
    string master_addr, master_port;

    openlog(NULL, 0, LOG_USER);
    syslog(LOG_INFO, "client started...");
    if (argc == 5) {
        master_addr = argv[3];
        master_port = argv[4];
    } else {
        master_addr = MASTER_NODE_ADDR;
        master_port = MASTER_NODE_PORT;
    }
    if (argc >= 3) {
       max_threads = atoi(argv[1]);    
       max_requests = atoi(argv[2]);
    } else {
       max_threads = MAX_NUM_CLIENTS;
       max_requests = MAX_NUM_REQUESTS;
    }
    for (int num = 0; num < max_threads; ++num) {
        auto th = std::thread([num, max_requests, master_addr, master_port]() { 
           clientTask(num, max_requests, master_addr, master_port);
        });
        th.detach();
    }
    getchar();
    closelog();
    return 0;
}
