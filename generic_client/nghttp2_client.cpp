#include <nghttp2/asio_http2.h>
#include <nghttp2/asio_http2_client.h>
#include <boost/asio/io_service.hpp>

#include <memory>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <glog/logging.h>

using boost::asio::ip::tcp;
using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::client;
using namespace std;

struct Stream {

   Stream(const request * req, uint32_t req_id): req(req), req_id(req_id) {}

   void setClosed() { closed = true; }

   void reqCancel(uint32_t ec) {
       req->cancel(ec);
   }

   bool isClosed() const { return closed; }

   const request * req;
   uint32_t req_id;
   bool closed {false};
};

class TestClient: public std::enable_shared_from_this<TestClient> {

 static const uint32_t WAIT_TIMEOUT = 2; /* seconds */
 static const uint32_t CANCEL_ERROR = 10;

 public:

     TestClient(const std::string & name, const std::string & port):
                server_name(name), server_port(port) {

                    std::thread io_thread([this]() {
                        cout << "io service started" << std::endl;
                        run();
                        cout << "io service exit" << std::endl;
                    });
                    io_service_thread = std::move(io_thread);
                    /* wait for io service to start in a separate timeout */
                    std::unique_lock<std::mutex> mlock(mutex_);
                    uint32_t timeout = WAIT_TIMEOUT;
                    cond_.wait_for(mlock, std::chrono::seconds(timeout),
                            [this](){return (thread_started) == true;});
      }

     ~TestClient() {
     }

     bool start(uint32_t timeout = 2) {
         return thread_started;
     }
    
     bool connect(uint32_t conn_timeout, uint32_t read_timeout) {

         auto self = shared_from_this();
         self->closed = false;

         self->sess.reset(new session(io_service, server_name, server_port));

         boost::posix_time::time_duration td = boost::posix_time::seconds(conn_timeout);
         self->sess->connect_timeout(td);
         td = boost::posix_time::seconds(read_timeout);
         self->sess->read_timeout(td);

         io_service.post(
                 [self]() {

                 self->sess->on_connect([self](tcp::resolver::iterator endpoint_it) {
                     LOG(INFO) << "operation=sess_connect_cb host=" << endpoint_it->endpoint().address().to_string()
                     << ":" << endpoint_it->endpoint().port() << std::endl;
                        self->status = true;
                 });

                 self->sess->on_error([self](const boost::system::error_code & ec) {
                     LOG(ERROR) << "operation=sess_error_cb ec=" << ec.message() << std::endl;
                     self->status = false;
                 });
            });
     }

     bool connected() const {  return status; }

     void onResponseCb(const response & res, const std::shared_ptr<Stream> & stream) {
        LOG(INFO) << "operation=onResponseCb" << std::endl;

        std::lock_guard<std::mutex> lg(mutex_);
        if (closed) {
            LOG(INFO) << "operation=onResponseCb already closed" << std::endl;
            return;
        }
        if (stream->isClosed()) { 
           LOG(INFO) << "operation=onResponseCb request already closed" << std::endl;
           return;
        }

        res.on_data([] (const uint8_t * data, size_t len) {
            if (len == 0) {
               cout << "Received response" << std::endl;
            }
        });
    }

    void onReqCloseCb(const std::shared_ptr<Stream> & stream, uint32_t req_id, int ec) {
         LOG(INFO) << "operation=onReqCloseCb ec=" << ec << " , req_id=" << req_id << std::endl;

         std::lock_guard<std::mutex> lg(mutex_);
         if (closed) {
             LOG(INFO) << "operation=onReqCloseCb client is already closed" << std::endl;
             return;
         }

         if (stream->isClosed()) {
             LOG(INFO) << "operation=onReqCloseCb request is already closed" << std::endl;
             return;
         }

         if (ec != 0) {
             LOG(ERROR) << "request id=" << req_id << " got an error, ec=" << ec << std::endl;
         }

         req_map.erase(req_id);
         LOG(INFO) << "operation=onReqCloseCb status=success" << std::endl;
     }

    int submitRequest(const std::string & method, const std::string & url, const std::string & data) {

        LOG(INFO) << "operation=submitRequest url=" << url << ", method=" << method << std::endl;

        auto client = shared_from_this();

        std::lock_guard<std::mutex> lg(mutex_);
        if (closed) {
            LOG(INFO) << "operation=submitRequest error=SessionAlreadyDisconnected " << std::endl;
            return -1;
        }
        /* submit the request */
        boost::system::error_code ec;
        auto req = client->sess->submit(ec, method, url, data);

        if (nullptr == req) {
            LOG(ERROR) << "operation=submitRequest ec=" << ec.value() << " , msg=" << ec.message() << std::endl;
            return -1;
        }
        uint32_t rid = ++client->id;
        std::shared_ptr<Stream> st = std::make_shared<Stream>(req, rid);

        /* store request information into map */
        client->req_map[rid] = st;
        /* on response call the response callback */
        req->on_response(
                [client, st](const response & res) {
                client->onResponseCb(res, st);
        });

        req->on_close(
                [client, rid, st](uint32_t ec) {
                client->onReqCloseCb(st, rid, ec);
        });

        return 0;
    }

    void submit(const std::string & data) {
        std::string url = "http://localhost:3333/"; 

        auto self = shared_from_this();
        auto & ios = self->sess->io_service();
        ios.post(
           [self, url, data]() {
                int ret = self->submitRequest("GET", url, data);
                if (ret != 0) {
                   LOG(ERROR) << "operation=submitGET error=submitRequest ret=" << static_cast<uint32_t>(ret) << std::endl;
                }
        });
     }

     void disconnect() {
         LOG(INFO) << "disconnect the session by cancelling all requests" << std::endl;

         std::lock_guard<std::mutex> lg(mutex_);
         if (closed) {
             LOG(WARNING) << "operation=disconnect already closed" << std::endl;
             return;
         }

         auto self = shared_from_this();
         auto & ios = self->sess->io_service();

         io_service.post(
                 [self] () {
                     if (self->req_map.size() == 0) {
                      self->sess->shutdown();
                      self->req_map.clear();
                      LOG(INFO) << "operation=disconnect session shutdown success" << std::endl;
                      return;
                    }

                    for (auto & e: self->req_map) {
                        auto st = e.second;
                        if (!st->isClosed()) {
                            st->reqCancel(TestClient::CANCEL_ERROR);
                        }
                    }
                    self->req_map.clear();
         });
         closed = true;

         LOG(INFO) << "operation=disconnect status=success" << std::endl;

     }
  
     void stop() {
        cout << "stopping client" << std::endl;
        io_service.stop(); 
        io_service_thread.join();
     }

 private:
    /*
       * io_thread which runs the underlying io_service.
       */
    void run() {
        LOG(INFO) << "operation=run io_service start" << std::endl;

        boost::asio::io_service::work work(io_service);
        thread_started = true;
        cond_.notify_one();

        io_service.run();

        thread_started = false;
        cond_.notify_one();
        LOG(INFO) << "operation=run io_service exit" << std::endl;
   }

   boost::asio::io_service io_service;
   bool thread_started {false};
   std::thread io_service_thread;
   std::mutex mutex_;
   std::condition_variable cond_;
   std::string server_name; 
   std::string server_port;
   std::shared_ptr<session> sess;
   bool status {false};
   std::map<uint32_t, std::shared_ptr<Stream>> req_map;
   bool closed {false};
   uint32_t id {0};
};

int main(int argc, char * argv[])
{
    /* server address port num request concurrency
     */
    if (argc != 5) {
        cout << "Usage:" << argv[0] << " server_name port num_reqs concurrency" << std::endl;
        return -1;
    }
    std::string name = argv[1];
    std::string port = argv[2];
    size_t num_reqs = atoi(argv[3]);
    size_t concurrency = atoi(argv[4]);

    std::vector<std::unique_ptr<std::thread>> clients;
    /* 
     * Invoke client 
     */
    for (size_t cnt = 0; cnt < concurrency; ++cnt) {
       clients.push_back(
               std::make_unique<std::thread>([num_reqs, name, port]() {
       std::shared_ptr<TestClient> client = std::make_shared<TestClient>(name, port);
       bool started = client->start();

       if (started) {
          client->connect(60, 60);
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          bool connected = client->connected();
          cout << "connect-status: " << connected << std::endl;
          if (connected) {
                std::string data = "some data";
                for (size_t cnt = 0; cnt < num_reqs; ++cnt) {
                    client->submit(data);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                client->disconnect();
         }
            client->stop();
        }
   }));
  }
  getchar();

  std::this_thread::sleep_for(std::chrono::seconds(1));
  for (auto & client_thread: clients) {
     client_thread->join();
  }
   
}
