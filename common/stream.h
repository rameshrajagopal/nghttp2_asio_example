#ifndef _STREAM_H_INCLUDED_
#define _STREAM_H_INCLUDED_ 

#include <cstdlib>
#include <utility>
#include <nghttp2/asio_http2_server.h>
#include <fstream>
#include <config.h>

using namespace std;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;

void writeRequestNum(int reqNum, header_map & h)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", reqNum);
    struct header_value hv = {buf, true};
    h.insert(std::make_pair("reqnum", hv));
}

void writeClientRequestNum(const request & req, header_map & h)
{
    auto search = req.header().find("clientreq");
    if (search != req.header().end()) {
    //    cout << search->second.value << endl;
        h.insert(std::make_pair(search->first, search->second));
    }
}

void writeDatasize(int size, header_map & h)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%d", size);
    struct header_value hv = {buf, true};
    h.insert(std::make_pair("size", hv));
}

void enableCompression(header_map & h)
{

   struct header_value hv = {"gzip", true};
   h.insert(std::make_pair("content-encoding", hv));
}

class FileData {
public:
    FileData(const char * filename) {
        ifstream is(filename);
        streampos begin, end;
        begin = is.tellg();
        is.seekg(0, ios::end);
        end = is.tellg();
        is.seekg(0, ios::beg);
        size = end - begin;
        buffer = new char[size];
        is >> buffer;
    }
    ssize_t read(uint8_t * data, size_t len) {
        if (curpos >= size) return 0;
        int ret = len;
        if ((size-curpos) > len) {
            memcpy(data, buffer + curpos, len);
        } else {
            ret = size - curpos;
            memcpy(data, buffer + curpos, ret);
        }
        curpos += ret;
        return ret;
    }
private:
    size_t size{0};
    size_t curpos{0};
    char * buffer;

};

struct Stream : public std::enable_shared_from_this<Stream> {
    Stream(const request &req, const response &res,
            boost::asio::io_service &io_service, int rnum)
        : io_service(io_service), req(req), res(res), 
          closed(false), req_num(rnum) {}
    void commit_result() {
        auto self = shared_from_this();
        io_service.post([self]() {
            header_map h;
            std::lock_guard<std::mutex> lg(self->mu);
            if (self->closed) {
               return;
            }
            writeRequestNum(self->req_num, h);
            writeClientRequestNum(self->req, h);
            writeDatasize(within(16 * 1024), h);
            enableCompression(h);
            self->res.write_head(200, h);
            self->res.end([](uint8_t * buf, size_t len, uint32_t * flags) -> ssize_t {
                  memset(buf, 'c', len);
                  *flags = NGHTTP2_DATA_FLAG_EOF;
                  return len;
                });
        });
    }
    int get_request_num() {
        return req_num;
    }
    void set_closed(bool f) {
        std::lock_guard<std::mutex> lg(mu);
        closed = f;
    }

    boost::asio::io_service &io_service;
    std::mutex mu;
    const request &req;
    const response &res;
    int req_num;
    bool closed;
};


#endif /*_STREAM_H_INCLUDED_*/
