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

extern void ReqRouterTask(Queue<shared_ptr<Stream>> & q, string configFile);

int getRequestNum(shared_ptr<Stream> st) 
{
    return st->get_request_num();
}

void sendResponse(shared_ptr<Stream> st)
{
    st->commit_result();
}

#define MAX_NUM_WORKER_THREADS (1)

int main(int argc, char *argv[]) 
{
    http2 server;
    std::atomic<int> reqNum {0};
    int numSlaves = 0;

    if (argc != 4) {
        cout << "Usage: " << endl;
        cout << argv[0] << " ip port slave_config_file" << endl;
        return -1;
    }

    openlog(NULL, 0, LOG_USER);
    string masterip = argv[1];
    string port = argv[2];
    string config = argv[3];
    cout << "Server started ip " << masterip << " port " << port << endl;
    syslog(LOG_INFO, "Server started ip: %s port: %s\n", masterip.c_str(), port.c_str());
    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

    auto th = std::thread([&q, config]() {
       ReqRouterTask(q, config);
    });
    th.detach();
    server.handle("/", [&q, &reqNum](const request & req, const response & res) {
        int cnt = reqNum++;
        cout << "req " << cnt << endl;
        auto & io_service = res.io_service();
        auto st = std::make_shared<Stream>(req, res, io_service, cnt);
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
    if (server.listen_and_serve(ec, masterip, port, true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.join();
    closelog();
}

