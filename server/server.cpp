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

extern void ReqRouterTask(Queue<shared_ptr<Stream>> & q, ConfigFile & cfg);

int getRequestNum(shared_ptr<Stream> st) 
{
    return st->get_request_num();
}

void sendResponse(shared_ptr<Stream> st, bool randomness, int bytes, bool compr)
{
    st->commit_result(randomness, bytes, compr);
}

#define MAX_NUM_WORKER_THREADS (1)

int main(int argc, char *argv[]) 
{
    http2 server;
    std::atomic<int> reqNum {0};
    int numSlaves = 0;

    if (argc != 2) {
        cout << "Usage: " << endl;
        cout << argv[0] << " master_config" << endl;
        return -1;
    }

    ConfigFile cfg(argv[1]);
    cout << "Master started with the below config: " << endl;
    cfg.printAll();
    openlog(NULL, 0, LOG_USER);
    string masterip = cfg.getValueOfKey<string>("masterip", "127.0.0.1");
    string port = cfg.getValueOfKey<string>("masterport", "8000");
    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

    auto th = std::thread([&q, &cfg]() {
       ReqRouterTask(q, cfg);
    });
    th.detach();
    server.handle("/", [&q, &reqNum](const request & req, const response & res) {
        int cnt = reqNum++;
        SYSLOG(LOG_INFO, "received req num %d\n", cnt);
        auto & io_service = res.io_service();
        auto st = std::make_shared<Stream>(req, res, io_service, cnt);
        res.on_close([st](uint32_t error_code) {
            st->set_closed(true);
        });
        req.on_data([&q, st](const uint8_t * data, size_t len) {
            cout << "received len " << len << endl;
            if (len == 0) {
               q.push(st);
            }
        });
    });
    server.handle("/hello", [](const request & req, const response & res) {
       res.write_head(200);
       res.end("Hello World page");
       res.on_close([](uint32_t ec) {
           cout << "request got closed" << endl;
       });
    });

    boost::system::error_code ec;
    if (server.listen_and_serve(ec, masterip, port, true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.join();
    closelog();
}

