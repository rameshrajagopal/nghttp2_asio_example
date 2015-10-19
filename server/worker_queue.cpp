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

class Request {
public:
    Request(const request & req, const response & res) :
        req_(req), res_(res) {}
    const response & getResponse() {
        return res_;
    }
private:
    const request & req_;
    const response & res_;
};

class Worker {
    public :
        Worker(const int num, Queue<Request *> & queue) : 
            threadNum(num), queue_(queue) {}

        void dowork(const response & res) {
            cout << "actual work" << endl;
            usleep(1000 * 100);
            res.write_head(200);
            cout << "sending reply" << endl;
            res.end("Hello world");
        }
        void run() {
            while (true) {
                Request * request = queue_.pop();
                dowork(request->getResponse());
            }
        }
private :
      int threadNum;
      Queue<Request *> & queue_;
};


int main(int argc, char *argv[]) {
    boost::system::error_code ec;
    http2 server;
    int reqNum = 0;
    Queue<Request *> queue;

    vector <thread*> threads;
    vector <Worker*> workers;
    for (int num = 0; num < MAX_NUM_WORKER_THREADS; ++num) {
        Worker * w = new Worker(num, queue);
        workers.push_back(w);
        threads.push_back(new thread(&Worker::run, w));
        threads[num]->detach();
    }
    server.handle("/work", [&reqNum, &queue](const request &req, const response &res) {
        int  cnt = reqNum++;
        cout << "received req: " << cnt << endl;
        queue.push(new Request(req, res));
    });
    if (server.listen_and_serve(ec, "localhost", "7000", true)) {
        std::cerr << "error: " << ec.message() << std::endl;
    }
    server.num_threads(2);
    server.join();
    for (int num = 0; num < MAX_NUM_WORKER_THREADS; ++num) {
        delete workers[num];
        delete threads[num];
    }
}
