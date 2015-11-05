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

    if (argc != 3) {
        cout << "Usage: " << endl;
        cout << argv[0] << " ip port " << endl;
        return -1;
    }
    openlog(NULL, 0, LOG_USER);
    slaveAddr = argv[1];
    slavePort = argv[2];
    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

    SYSLOG(LOG_INFO, "worker started with %d threads\n", MAX_NUM_WORKER_THREADS);
    for (int num = 0; num < MAX_NUM_WORKER_THREADS; ++num) {
        auto th = std::thread([&q]() {
            srandom((unsigned) time(NULL));
            for (;;) {
                auto st = q.pop();
                /* do actual work */
                usleep(100 * 1000);
                st->commit_result();
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
