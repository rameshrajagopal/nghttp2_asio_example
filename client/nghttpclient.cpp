#include <iostream>
#include <thread>
#include <nghttp2/asio_http2_client.h>
#include <config.h>
#include <syslog.h>
#include <mutex>

using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

#define REQUEST_SIZE (1024)
#define MAX_NUM_CLIENTS  (100)
#define MAX_NUM_REQUESTS (1000)

#ifdef ENABLE_SYSLOG
#define  SYSLOG(fmt...) syslog(fmt)
#else
#define  SYSLOG(fmt...) syslog(fmt)
#endif


struct timeval getDiffTime(struct timeval tstart, struct timeval tend)
{
struct timeval tdiff;

if (tend.tv_usec < tstart.tv_usec) {
    tdiff.tv_sec = tend.tv_sec - tstart.tv_sec - 1;
    tdiff.tv_usec = (1000000 - tstart.tv_usec) + tend.tv_usec;
} else {
    tdiff.tv_sec = tend.tv_sec - tstart.tv_sec;
    tdiff.tv_usec = tend.tv_usec - tstart.tv_usec;
}
return tdiff;
}

typedef struct {
int min_value;
int max_value;
int failures;
int64_t total_time;
}timing_t;

void printStats(map<string, struct timeval> & reqMap, timing_t & time)
{
int min_value = INT_MAX, max_value = 0;
string min_req, max_req;
int avg = 0, total = 0, num_requests = 0;
int64_t total_msec = 0;
int nerrors = 0;

auto it = reqMap.begin();
for (; it != reqMap.end(); ++it) {
    total_msec = (it->second.tv_sec * 1000) + (it->second.tv_usec/1000);
    if (total_msec == 0) ++nerrors;
    else {
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
}
    time.total_time += total_msec;
    if (min_value < time.min_value) {
        time.min_value = min_value;
    }
    if (max_value > time.max_value) {
        time.max_value = max_value;
    }
    time.failures += nerrors;
}

void clientTask(int clientNum, int max_requests, 
                string master_addr, string master_port,
                map<string, struct timeval> & reqMap
                )
{
    boost::system::error_code ec;
    boost::asio::io_service io_service;
    std::mutex mutex_;

    SYSLOG(LOG_INFO, "client task: %d connecting with: %s:%s", clientNum, master_addr.c_str(), master_port.c_str());
    session sess(io_service, master_addr, master_port);
    map<string, int> clientMap;
    sess.on_connect([&sess, clientNum, max_requests, &clientMap, &reqMap, &mutex_](tcp::resolver::iterator endpoint_it) {
            boost::system::error_code ec;
            auto printer = [&clientMap, &reqMap](const response &res) {
                string key;
                cout << "response " << endl;
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
            std::atomic<std::size_t> max_cnt(max_requests);
            auto count = make_shared<int>(max_cnt);
            char buf[16];

            struct timeval tstart, curtime;
            gettimeofday(&tstart, NULL);
            auto startPtr = make_shared<struct timeval>(tstart);
            srandom((unsigned) time(NULL));
            for (size_t i = 0; i < max_requests; ++i) {
            header_map h;
            snprintf(buf, sizeof(buf), "%d:%ld", clientNum, i);
            struct header_value hv = {buf, true};
            h.insert(make_pair("clientreq", hv));
            clientMap[buf] = 0;
            gettimeofday(&curtime, NULL);
            SYSLOG(LOG_INFO, "request %s sec:%ld usec:%ld\n", buf, curtime.tv_sec, curtime.tv_usec);
            reqMap[buf] = curtime;
            auto data_generator = []
                   (uint8_t * buf, size_t len, uint32_t * flags) -> ssize_t {
                       cout << "data is getting copied" << endl;
                       memset(buf, 'a', REQUEST_SIZE);
                       *flags = NGHTTP2_DATA_FLAG_EOF;
                       return REQUEST_SIZE;
            };

            char data[1024];
            memset(data, 'c', 1024);
            auto req = sess.submit(ec, "POST", MASTER_NODE_URI, data, h);
            req->on_response(printer);
            req->on_close([&sess, count, startPtr, clientNum, &mutex_](uint32_t error_code) {
                std::unique_lock<std::mutex> mlock(mutex_);
                int tmpCnt = --*count;
                mlock.unlock();
                if (tmpCnt == 0) {
                    cout << "tempcnt has come down \n" << endl;
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
//                    sess.shutdown();
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

    if (argc != 5) {
        cout << "usage: " << endl;
        cout << argv[0] << "  masterip port concurrent repeat" << endl;
        return -1;
    }
    openlog(NULL, 0, LOG_USER);
    SYSLOG(LOG_INFO, "client started...");
    master_addr = argv[1];
    master_port = argv[2];
    max_threads = atoi(argv[3]);    
    max_requests = atoi(argv[4]);
    std::map<string, struct timeval> reqMaps[max_threads];
    for (int num = 0; num < max_threads; ++num) {
        auto th = std::thread([num, max_requests, master_addr, master_port, &reqMaps]() { 
           clientTask(num, max_requests, master_addr, master_port, reqMaps[num]);
        });
        th.detach();
    }
    getchar();
    timing_t time;
    time.min_value = INT_MAX;
    time.max_value = INT_MIN;
    time.failures = 0;
    time.total_time = 0;
    for (int num = 0; num < max_threads; ++num) {
       printStats(reqMaps[num], time);
    }
    cout << "Total requests " << max_requests * max_threads << endl;
    cout << "min val: " << time.min_value << endl;
    cout << "max val: " << time.max_value << endl;
    cout << "total time: " << time.total_time << endl;
    cout << "total errors: " << time.failures << endl;
    closelog();
    return 0;
}
