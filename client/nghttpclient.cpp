#include <iostream>
#include <thread>
#include <nghttp2/asio_http2_client.h>
#include <syslog.h>

using boost::asio::ip::tcp;

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;

#define MAX_NUM_CLIENTS  (2)
#define MAX_NUM_REQUESTS (10)

void clientTask(int clientNum)
{
    boost::system::error_code ec;
    boost::asio::io_service io_service;

    syslog(LOG_INFO, "client task: %d", clientNum);
    session sess(io_service, "localhost", "8000");
    sess.on_connect([&sess, clientNum](tcp::resolver::iterator endpoint_it) {
            boost::system::error_code ec;

            auto printer = [](const response &res) {
            res.on_data([](const uint8_t * data, size_t len) {
                syslog(LOG_INFO, "received data %d\n", (int)len);
                });
            };
            std::size_t num = MAX_NUM_REQUESTS;
            auto count = make_shared<int>(num);

            struct timeval tstart;
            gettimeofday(&tstart, NULL);
            auto startPtr = make_shared<struct timeval>(tstart);

            for (size_t i = 0; i < num; ++i) {
            auto req = sess.submit(ec, "GET", "http://localhost:8000/");
            cout << "sent... " << num << endl;
            req->on_response(printer);
            req->on_close([&sess, count, startPtr, clientNum](uint32_t error_code) {
                syslog(LOG_INFO, "response Num: %d clientNum: %d\n", *count, clientNum);
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
                    int total_msec = (tdiff.tv_sec * 1000) + (tdiff.tv_usec/1000);
                    syslog(LOG_INFO, "client: %d total msec: %d\n", clientNum, total_msec);
                    sess.shutdown();
                }
                });
            }
    });

    sess.on_error([clientNum](const boost::system::error_code &ec) {
            syslog(LOG_INFO, "session connection error: %d\n", clientNum);
            });

    io_service.run();
}

int main(int argc, char *argv[])
{
    openlog(NULL, 0, LOG_USER);
    syslog(LOG_INFO, "client started...");
    for (int num = 0; num < MAX_NUM_CLIENTS; ++num) {
        auto th = std::thread([&num]() { 
           clientTask(num);
        });
        th.detach();
    }
    getchar();
    closelog();
    return 0;
}
