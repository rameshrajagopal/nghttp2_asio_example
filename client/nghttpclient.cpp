#include <iostream>
#include <nghttp2/asio_http2_client.h>

using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

int main(int argc, char *argv[])
{
    boost::system::error_code ec;
    boost::asio::io_service io_service;

    session sess(io_service, "localhost", "7000");

    sess.on_connect([&sess](tcp::resolver::iterator endpoint_it) {
            boost::system::error_code ec;

            auto printer = [](const response &res) {
            res.on_data([](const uint8_t * data, size_t len) {
                cout.write(reinterpret_cast<const char *>(data), len);
                cout << endl;
                });
            };
            std::size_t num = 10;
            auto count = make_shared<int>(num);

            struct timeval tstart;
            auto startPtr = make_shared<struct timeval>(tstart);
            gettimeofday(&tstart, NULL);
            cout << tstart.tv_sec << " " << tstart.tv_usec << endl;

            for (size_t i = 0; i < num; ++i) {
            auto req = sess.submit(ec, "GET", "http://localhost:7000/work");
            cout << "sent... " << num << endl;
            req->on_response(printer);
            req->on_close([&sess, count, startPtr](uint32_t error_code) {
                    uint64_t total_sec = 0;
                    if (--*count == 0) {
                    struct timeval tend;
                    gettimeofday(&tend, NULL);
                    cout << tend.tv_sec << " " << tend.tv_usec << endl;
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
