#include <iostream>
#include <vector>
#include <list>
#include <thread>
#include <memory>
#include <nghttp2/asio_http2_server.h>

#include "Queue.cpp"

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;


#define MAX_NUM_WORKER_THREADS (10)

struct Stream : public std::enable_shared_from_this<Stream> {
    Stream(const request &req, const response &res,
            boost::asio::io_service &io_service)
        : io_service(io_service), req(req), res(res), closed(false) {}
    void commit_result() {
        auto self = shared_from_this();
        io_service.post([self]() {
            std::lock_guard<std::mutex> lg(self->mu);
            if (self->closed) {
               return;
            }
            self->res.write_head(200);
            self->res.end("done");
        });
    }
    void set_closed(bool f) {
        std::lock_guard<std::mutex> lg(mu);
        closed = f;
    }

    boost::asio::io_service &io_service;
    std::mutex mu;
    const request &req;
    const response &res;
    bool closed;
};

int main(int argc, char *argv[]) {
    http2 server;

    server.num_threads(2);
    Queue<shared_ptr<Stream>> q;

    for (int num = 0; num < MAX_NUM_WORKER_THREADS; ++num) {
        auto th = std::thread([&q]() {
            for (;;) {
                auto st = q.pop();
                /* do actual work */
                usleep(100 * 1000);
                st->commit_result();
            }
        });
        th.detach();
    }
    server.handle("/work", [&q](const request & req, const response & res) {
        cout << "received req " << endl;
        auto & io_service = res.io_service();
        auto st = std::make_shared<Stream>(req, res, io_service);
        res.on_close([st](uint32_t error_code) {
            st->set_closed(true);
        });
        q.push(st);
    });

    boost::system::error_code ec;
    if (server.listen_and_serve(ec, "localhost", "7000", true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.join();
}
