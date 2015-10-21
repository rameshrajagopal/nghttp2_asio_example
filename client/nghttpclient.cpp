#include <iostream>
#include <thread>
#include <nghttp2/asio_http2_client.h>

using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

#define MAX_NUM_CLIENTS  (2)

void clientTask(int num)
{
    boost::system::error_code ec;
    boost::asio::io_service io_service;

    session sess(io_service, "localhost", "8000");
    auto clientNum = make_shared<int> (num);
    sess.on_connect([&sess, clientNum](tcp::resolver::iterator endpoint_it) {
            boost::system::error_code ec;

            auto printer = [](const response &res) {
            res.on_data([](const uint8_t * data, size_t len) {
                cout.write(reinterpret_cast<const char *>(data), len);
                cout << endl;
                });
            };
            std::size_t num = 1000;
            auto count = make_shared<int>(num);

            struct timeval tstart;
            gettimeofday(&tstart, NULL);
            auto startPtr = make_shared<struct timeval>(tstart);

            for (size_t i = 0; i < num; ++i) {
            auto req = sess.submit(ec, "GET", "http://localhost:8000/");
            cout << "sent... " << num << endl;
            req->on_response(printer);
            req->on_close([&sess, count, startPtr, clientNum](uint32_t error_code) {
                cout << "response got : " << *count << endl;
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
                cout << "client: " << *clientNum << " " << (tdiff.tv_sec * 1000) + (tdiff.tv_usec/1000) << " msec";
                sess.shutdown();
                }
                });
            }
    });

    sess.on_error([](const boost::system::error_code &ec) {
            std::cerr << "error: " << ec.message() << std::endl;
            });

    io_service.run();
}

int main(int argc, char *argv[])
{
    for (int num = 0; num < MAX_NUM_CLIENTS; ++num) {
        auto th = std::thread([&num]() { 
           clientTask(num);
        });
        th.detach();
    }
    getchar();
    return 0;
}
