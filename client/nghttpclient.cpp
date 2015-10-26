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

#ifdef ENABLE_SYSLOG
#define  SYSLOG(fmt...) syslog(fmt)
#else
#define  SYSLOG(fmt...) 
#endif

#define within(num) (int) ((float) num * random() / (RAND_MAX + 1.0))

struct timeval getDiffTime(struct timeval tstart, struct timeval tend)
{
    struct timeval tdiff;

    if (tend.tv_usec < tstart.tv_usec) {
        tdiff.tv_sec = tend.tv_sec - tstart.tv_sec - 1;
        tdiff.tv_usec = 1000000 + tend.tv_usec - tstart.tv_usec;
    } else {
        tdiff.tv_sec = tend.tv_sec - tstart.tv_sec;
        tdiff.tv_usec = tend.tv_usec - tstart.tv_usec;
    }
    return tdiff;
    //int total_msec = (tdiff.tv_sec * 1000) + (tdiff.tv_usec/1000);
}

void printStats(map<string, struct timeval> & reqMap)
{
    int min_value = INT_MAX, max_value = 0;
    string min_req, max_req;
    int avg = 0, total = 0, num_requests = 0;
    int total_msec = 0;

    auto it = reqMap.begin();
    for (; it != reqMap.end(); ++it) {
//        syslog(LOG_INFO, "%s:  %ld %d\n", it->first, it->second.tv_sec, it->second.tv_usec);
        total_msec = (it->second.tv_sec * 1000) + (it->second.tv_usec/1000);
        total += total_msec; 
        ++num_requests;
        if (total_msec < min_value) {
            min_value = total_msec;
            min_req = it->first;
        }
        if (total_msec > max_value) {
            max_value = total_msec;
            max_req = it->first;
        }
    }
    cout << "min req: " << min_req << " value " << min_value << " msec" << endl;
    cout << "max req: " << max_req << " value " << max_value << " msec" << endl;
    cout << "total time: " << total << " total requests: " << num_requests << endl;
    cout << "avg time: " << (total/num_requests) << endl;
}

void clientTask(int clientNum, int max_requests, 
                string master_addr, string master_port,
                map<string, struct timeval> & reqMap
                )
{
    boost::system::error_code ec;
    boost::asio::io_service io_service;

    SYSLOG(LOG_INFO, "client task: %d", clientNum);
    session sess(io_service, master_addr, master_port);
    map<string, int> clientMap;
    sess.on_connect([&sess, clientNum, max_requests, &clientMap, &reqMap](tcp::resolver::iterator endpoint_it) {
            boost::system::error_code ec;
            auto printer = [&clientMap, &reqMap](const response &res) {
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
                res.on_data([&clientMap, key, &reqMap](const uint8_t * data, size_t len) {
                   clientMap[key] -= len;
                   SYSLOG(LOG_INFO, "data from server of len: %ld\n", len);
                   if (len == 0) {
                       struct timeval curtime;
                       gettimeofday(&curtime, NULL);
                       reqMap[key.c_str()] = getDiffTime(reqMap[key.c_str()], curtime);
                       SYSLOG(LOG_INFO, "response %s sec:%ld usec:%ld\n", key.c_str(), curtime.tv_sec, curtime.tv_usec);
                   }
                });
            };
            std::size_t num = max_requests;
            auto count = make_shared<int>(num);
            char buf[16];

            struct timeval tstart, curtime;
            gettimeofday(&tstart, NULL);
            auto startPtr = make_shared<struct timeval>(tstart);
            srandom((unsigned) time(NULL));
            for (size_t i = 0; i < num; ++i) {
            header_map h;
            snprintf(buf, sizeof(buf), "%d:%ld", clientNum, i);
            struct header_value hv = {buf, true};
            h.insert(make_pair("clientreq", hv));
            clientMap[buf] = 0;
            gettimeofday(&curtime, NULL);
            SYSLOG(LOG_INFO, "request %s sec:%ld usec:%ld\n", buf, curtime.tv_sec, curtime.tv_usec);
            reqMap[buf] = curtime;
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
                    SYSLOG(LOG_INFO, "client: %d total msec: %d\n", clientNum, total_msec);
                    sess.shutdown();
                }
                });
             usleep(within(400 * 1000));//400 msec
            }
    });

    sess.on_error([clientNum](const boost::system::error_code &ec) {
            SYSLOG(LOG_INFO, "session connection error: %d\n", clientNum);
            });

    io_service.run();
}

int main(int argc, char *argv[])
{
    int max_threads, max_requests;
    string master_addr, master_port;

    openlog(NULL, 0, LOG_USER);
    SYSLOG(LOG_INFO, "client started...");
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
    std::map<string, struct timeval> reqMaps[max_threads];
    for (int num = 0; num < max_threads; ++num) {
        auto th = std::thread([num, max_requests, master_addr, master_port, &reqMaps]() { 
           clientTask(num, max_requests, master_addr, master_port, reqMaps[num]);
        });
        th.detach();
    }
    getchar();
    for (int num = 0; num < max_threads; ++num) {
       printStats(reqMaps[num]);
    }
    closelog();
    return 0;
}
