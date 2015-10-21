#include <iostream>
#include <vector>
#include <list>
#include <thread>
#include <memory>
#include <nghttp2/asio_http2_server.h>

#include "queue.h"
#include "stream.h"


using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;

extern void SlaveTask(Queue<shared_ptr<Stream>> & q);

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
    volatile int reqNum = 0;

    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

#if 1
    auto th = std::thread([&q]() {
            SlaveTask(q);
    });
    th.detach();
#endif
    server.handle("/", [&q, &reqNum](const request & req, const response & res) {
        int cnt = reqNum++;
        cout << "received req " << cnt << endl;
        auto & io_service = res.io_service();
        auto st = std::make_shared<Stream>(req, res, io_service, cnt);
        res.on_close([st](uint32_t error_code) {
            st->set_closed(true);
        });
        q.push(st);
    });

    boost::system::error_code ec;
    if (server.listen_and_serve(ec, "localhost", "8000", true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.join();
}
