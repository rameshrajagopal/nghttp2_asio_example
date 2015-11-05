#include <iostream>
#include <vector>
#include <list>
#include <thread>
#include <memory>
#include <nghttp2/asio_http2_server.h>
#include <syslog.h>
#include <queue.h>
#include <stream.h>
#include <config.h>

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;


#define MAX_NUM_WORKER_THREADS (10)

int main(int argc, char *argv[]) {
    http2 server;
    string slaveAddr;
    string slavePort;
    std::atomic<int> reqNum {0};

    if (argc != 2) {
        cout << "Usage: " << endl;
        cout << argv[0] << " slave.cfg" << endl;
        return -1;
    }
    openlog(NULL, 0, LOG_USER);
    ConfigFile cfg(argv[1]);
    cfg.printAll();
    slaveAddr = cfg.getValueOfKey<string>("slave_addr");
    slavePort = cfg.getValueOfKey<string>("slave_port");
    int num_threads = cfg.getValueOfKey<int>("num_threads");
    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

    SYSLOG(LOG_INFO, "worker started with %d threads\n", num_threads);
    for (int num = 0; num < num_threads; ++num) {
        auto th = std::thread([&q, &cfg]() {
            srandom((unsigned) time(NULL));
            for (;;) {
                auto st = q.pop();
                /* do actual work */
                int time_to_sleep = cfg.getValueOfKey<int>("sleep_time");
                bool isRandom = cfg.getValueOfKey<string>("randomness") == "yes" ? true : false;
                usleep( isRandom ? within(time_to_sleep) : time_to_sleep);
                int num_bytes = cfg.getValueOfKey<int>("reply_bytes");
                bool compression = cfg.getValueOfKey<string>("compression") == "yes" ? true : false;
                st->commit_result(num_bytes, compression, isRandom);
            }
        });
        th.detach();
    }
    server.handle("/work", [&q, &reqNum](const request & req, const response & res) {	
        int cnt = reqNum++;
        cout << "reqNum " << reqNum << endl;
#if 0
        for (auto &kv : req.header()) {
           cout << kv.first << ":" << kv.second.value << endl;
        }
#endif
        auto search = req.header().find("reqnum");
        int reqNum = 0;
        if (search != req.header().end()) {
           reqNum = std::stoi(search->second.value, nullptr, 10);
           SYSLOG(LOG_INFO, "received request %d\n", reqNum);
        } else {
           SYSLOG(LOG_INFO, "invalid request, doesn't have reqNum\n");
        }
        auto & io_service = res.io_service();
        auto st = std::make_shared<Stream>(req, res, io_service, reqNum);
        res.on_close([st](uint32_t error_code) {
            st->set_closed(true);
        });
        req.on_data([&q, st](const uint8_t * data, size_t len) {
            if (len == 0) {
               q.push(st); 
            }
        });
    });

    boost::system::error_code ec;
    if (server.listen_and_serve(ec, slaveAddr, slavePort, true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.join();
}
