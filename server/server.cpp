#include <iostream>
#include <vector>
#include <list>
#include <thread>
#include <memory>
#include <nghttp2/asio_http2_server.h>
#include <syslog.h>
#include "queue.h"
#include "stream.h"
#include "config.h"


using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;

extern void ReqRouterTask(Queue<shared_ptr<Stream>> & q, int numSlaves);

int getRequestNum(shared_ptr<Stream> st) 
{
    return st->get_request_num();
}

void sendResponse(shared_ptr<Stream> st)
{
    st->commit_result();
}

#define MAX_NUM_WORKER_THREADS (1)

int main(int argc, char *argv[]) {
    http2 server;
    std::atomic<int> reqNum {0};
    int numSlaves = 0;

    openlog(NULL, 0, LOG_USER);
    syslog(LOG_INFO, "server started...\n");
    if (argc == 2) {
        numSlaves = atoi(argv[1]);
    } else {
        numSlaves = 2;
    }
    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

    auto th = std::thread([&q, numSlaves]() {
       ReqRouterTask(q, numSlaves);
    });
    th.detach();
    server.handle("/", [&q, &reqNum](const request & req, const response & res) {
        int cnt = reqNum++;
#if 0
        for (auto &kv: req.header()) {
            cout << kv.first << " " << kv.second.value << endl;
        }
#endif
        auto & io_service = res.io_service();
        auto st = std::make_shared<Stream>(req, res, io_service, cnt);
        res.on_close([st](uint32_t error_code) {
            st->set_closed(true);
        });
        q.push(st);
    });

    boost::system::error_code ec;
    if (server.listen_and_serve(ec, MASTER_NODE_ADDR, MASTER_NODE_PORT, true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.join();
    closelog();
}

