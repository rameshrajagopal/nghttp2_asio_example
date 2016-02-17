#include "cluster_server.h"
#include "cluster_common.h"
#include "status.h"
#include "concurrent_queue.h"
#include "cluster_host.h"
#include "cluster_utils.h"
#include "io_service.h"

#include <vector>
#include <sys/types.h>
#include <sys/wait.h>

#define GTEST_HAS_TR1_TUPLE 0
#include "gtest/gtest.h"
#include <glog/logging.h>

using EndpointHandler = rpc::server::EpHandler;
using ServerRpcResponse = rpc::server::RpcResponse;
using ServerRpcRequest  = rpc::server::RpcRequest;
using namespace cluster;
using namespace std::placeholders;

class EchoRequest {
  public:
      explicit EchoRequest(ServerRpcRequest & req, ServerRpcResponse & res):
            actual_req(std::move(req)), actual_res(std::move(res)) {}

      EchoRequest(const EchoRequest &) = delete;

      EchoRequest & operator=(const EchoRequest &) = delete;

      size_t readRequest(HeaderMap & hdr, Buffer<uint8_t> & payload) {

          actual_req.readHeader(hdr);
          size_t payload_length = actual_req.payloadLength();
          LOG(INFO) << "operation=EchoReqReceiveCb payload_length=" << payload_length << std::endl;

          /* now read the data */
          uint8_t * data = new uint8_t[payload_length];
          size_t ret_size = 0, offset = 0;
          size_t remain = payload_length;

          while (offset < payload_length) {
            ret_size = actual_req.readPayload(offset, remain, data + offset);
            offset += ret_size;
            remain -= ret_size;
          }
          payload.write(data, payload_length);
          delete[] data;

          return payload_length;
      }
      
      Status writeResponse(HeaderMap & hdr, Buffer<uint8_t> & payload) {  

          actual_res.writeHeader(hdr);
          actual_res.writePayload(std::move(payload));

          return Status::SUCCESS;
      }

      Status submitResponse(Server & rpc_server, Status status) {

          Status ret = rpc_server.submitResponse(std::move(actual_res), status);
          if (ret != Status::SUCCESS) {
              LOG(INFO) << "operation=processEchoRequest failed to submit ret=" << static_cast<uint32_t>(ret) << std::endl;
          }

          return ret;
      }

  private:
      ServerRpcRequest actual_req;
      ServerRpcResponse actual_res;
};

class EchoResponse {
  public:
      explicit EchoResponse(RpcClientResponse & res, const RpcStatus rc, uint8_t * src, size_t len):
                            server_res(std::move(res)), status(rc), source(src), length(len) {}

      EchoResponse(const EchoResponse &) = delete;

      EchoResponse & operator=(const EchoResponse &) = delete;

      void responseVerify() {
         LOG(INFO) << "operation=echoResponseReceiveCb status=" << static_cast<uint32_t>(status) << std::endl;

         EXPECT_EQ(status, RpcStatus::SUCCESS);
         if (status == RpcStatus::SUCCESS) {
             size_t buf_length = server_res.payloadLength();
             EXPECT_EQ(length, buf_length);

             uint8_t * data = new uint8_t[buf_length];
             size_t ret = server_res.readPayload(0, buf_length, data);
             EXPECT_EQ(ret, buf_length);

             EXPECT_EQ(memcmp(source, data, buf_length), 0);
             delete[] data;
         }
      }
  private:
      RpcClientResponse server_res;
      RpcStatus status;
      uint8_t * source;
      size_t  length;
};

class EchoClient {
 public:
     EchoClient(size_t num):response_thread(std::bind(&EchoClient::responseVerifyThread, this)), num_requests(num) {
         started = ios.start();
         if (!started) {
             LOG(ERROR) << "operation=createMasterSession error=IoServiceStart error" << std::endl;
         }
     }

     ~EchoClient() { 
     }

     Status createSession(const std::string & host_name, const uint16_t port,
                          uint32_t conn_timeout, uint32_t read_timeout,
                          std::shared_ptr<Host> & clientHost) {

         HostMeta meta(host_name, port, HostType::SLAVE);
         clientHost = std::make_shared<Host>(meta);

         Status ret = clientHost->createSession(ios, conn_timeout, read_timeout);
         if (ret != Status::SUCCESS) {
             LOG(INFO) << "operation=createSession ret=" << static_cast<uint32_t>(ret) << std::endl;
             return ret;
         }

         return Status::SUCCESS;
     }

     void responseVerifyThread() {
        size_t num_res_received = 0;
        while (true) {
            std::shared_ptr<EchoResponse> echo_res;

            if (res_q.pop(echo_res) == -1) {
                LOG(INFO) << "operation=responseVerifyThread exiting" << std::endl;
                break;
            }
            if (!started) {
                LOG(INFO) << "operation=responseVerifyThread exiting" << std::endl;
                break;
            }
            LOG(INFO) << "operation=responseVerifyThread response received" << std::endl;
            echo_res->responseVerify();
            ++num_res_received;
            if (num_res_received == num_requests) {
                break;
            }
        }
        LOG(INFO) << "operation=responseVerifyThread exiting" << std::endl;
     }

     Status submitRequest(const std::shared_ptr<Host> & slaveHost, const std::string & host_name,
                          const uint16_t port, uint8_t * source, size_t length) {
         /* header map */
         HeaderMap hdr;
         utils::setHeaderValues(RpcHeader::METHOD_POST, RpcHeader::SCHEME_HTTP, host_name,
                                std::to_string(port), "/echo", RpcHeader::BINARY_PAYLOAD, hdr);

         /* request */
         RpcClientRequest rpc_req;
         rpc_req.writeHeader(hdr);
         rpc_req.writePayload(source, length);

         /* submit request */
         Status ret = slaveHost->submitRequest(rpc_req, [this, source, length](
                     RpcClientResponse res, const RpcStatus status) {
            /*verify here */
            res_q.push(std::make_shared<EchoResponse>(res, status, source, length));
         });
         if (ret != Status::SUCCESS) {
             LOG(INFO) << "operation=submitRequest ret=" << static_cast<uint32_t>(ret) << std::endl;
             return ret;
         }

         return Status::SUCCESS;
     }

     Status deleteSession(const std::shared_ptr<Host> & slaveHost) {
         Status ret = slaveHost->disconnect();
         if (ret != Status::SUCCESS) {
             LOG(INFO) << "operation=deleteSession ret=" << static_cast<uint32_t>(ret) << std::endl;
             return ret;
         }

         return Status::SUCCESS;
     }

     void stop() {
         LOG(INFO) << "operation=stop" << std::endl;
         started = false;
         res_q.destroy();
         std::this_thread::sleep_for(std::chrono::milliseconds(100));
         if (response_thread.joinable()) response_thread.join();
         LOG(INFO) << "stopping io service" << std::endl;
         ios.stop();
         LOG(INFO) << "operation=stop exit" << std::endl;
     }
 private:
   IOService ios; 
   bool started {false};
   std::mutex mutex_;
   std::condition_variable cond_;
   std::thread response_thread;
   ConcurrentQueue<std::shared_ptr<EchoResponse>> res_q;
   size_t num_requests;
};

void client_thread(EchoClient & client, size_t num_requests, const std::string & host_name, uint16_t port) {
    uint32_t conn_timeout = 2;
    uint32_t read_timeout = 60;

    std::shared_ptr<Host> clientHost;
    Status ret = client.createSession(host_name, port, conn_timeout,
            read_timeout, clientHost);
    EXPECT_EQ(ret, Status::SUCCESS);
    if (ret != Status::SUCCESS) {
        LOG(ERROR) << "operation=connect failed to connect" << std::endl;
        return;
    }
    /* submit request */
    size_t buf_length = 8 * 1024;
    uint8_t * source = new uint8_t[buf_length];
    memset(source, 'a', buf_length);

    for (size_t num = 0; num < num_requests; ++num) {
        ret = client.submitRequest(clientHost, host_name, port, source, buf_length);
        EXPECT_EQ(ret, Status::SUCCESS);
        if (ret != Status::SUCCESS) {
            LOG(ERROR) << "operation=submitrequest error=SubmitRequestError num_active_requests=" 
                << clientHost->numActiveRequests() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        LOG(INFO) << "operation=submitRequestNverify counter=" << num << std::endl;
    }
    /* disconnect  */
    ret = client.deleteSession(clientHost);
    EXPECT_EQ(ret, Status::SUCCESS);
    if (ret != Status::SUCCESS) {
        return;
    }
    delete[] source;
}

int main(int argc, char * argv[]) {

    std::string host_name = "localhost";
    uint16_t port = 3333;
    uint32_t num_requests = 30;
    uint32_t num_clients = 4;

    if (argc == 5) {
        host_name = argv[1];
        port = atoi(argv[2]);
        num_requests = atoi(argv[3]);
        num_clients = atoi(argv[4]);
    }
    /* client process */
    int client_pid = fork();
    if (client_pid == 0) {
        EchoClient client(num_requests);

        std::vector<std::thread *> client_sessions;

        for (size_t num = 0; num < num_clients; ++num) {
            client_sessions.push_back(new std::thread(
                        [&client, num_requests, host_name, port]() {
                client_thread(client, num_requests, host_name, port);
            }));
        }

        for (size_t num = 0; num < num_clients; ++num) {
            client_sessions[num]->join();
            delete client_sessions[num];
        }

        client.stop();
        return 0;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    int status = 0;
    wait(&status);
    return 0;
}
