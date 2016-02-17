#include <nghttp2_rpc_client_impl.h>
#include <nghttp2_client_stream.h>
#include <nghttp2_error.h>
#include <glog/logging.h>

using boost::asio::ip::tcp;
namespace rpc {
namespace client {

RpcClientImpl::RpcClientImpl() {}

RpcClientImpl::~RpcClientImpl() {
    req_map.clear();
}

RpcStatus RpcClientImpl::getRpcStatus(const boost::system::error_code & ec) {
   DLOG(INFO) << "operation=getRpcStatus ec=" << ec.value() << " msg=" << ec.message() << std::endl;

   BoostError boost_ec = static_cast<BoostError>(ec.value());
   switch (boost_ec) {
       case BoostError::HOST_CONNECTION_REFUSED: return RpcStatus::HOST_CONNECTION_REFUSED;

       case BoostError::HOST_EOF_ERROR: return RpcStatus::EOF_RESPONSE_ERROR;

       case BoostError::HOST_CONNECTION_RESET_BY_PEER: return RpcStatus::HOST_CONNECTION_RESET_BY_PEER;

       case BoostError::HOST_CONNECTION_TIMED_OUT: return RpcStatus::HOST_CONNECTION_TIMED_OUT;

       default: break;
   }

   return RpcStatus::HOST_CONNECTION_REFUSED;
}

RpcStatus RpcClientImpl::getRpcStatus(int ec) {
   DLOG(INFO) << "operation=getRpcStatus ec=" << ec << std::endl;

   Nghttp2Error nghttp2_ec = static_cast<Nghttp2Error>(ec);
   switch (nghttp2_ec) {
       case Nghttp2Error::SUCCESS: return RpcStatus::SUCCESS;

       case Nghttp2Error::INTERNAL_ERROR: return RpcStatus::INTERNAL_PROTOCOL_ERROR;

       case Nghttp2Error::HTTP_RES_SUCCESS: return RpcStatus::SUCCESS;

       case Nghttp2Error::HTTP_RES_NOTFOUND: return RpcStatus::REMOTE_METHOD_NOT_FOUND;

       case Nghttp2Error::HTTP_RES_INTERNAL_ERROR: return RpcStatus::REMOTE_SERVER_INTERNAL_ERROR;

       case Nghttp2Error::HTTP_BAD_REQUEST: return RpcStatus::INVALID_REQUEST;

       case Nghttp2Error::HTTP_RES_ACCEPTED: return RpcStatus::REQ_ACCEPTED;

       default: break;
   }

   return RpcStatus::INTERNAL_PROTOCOL_ERROR;
}

RpcStatus RpcClientImpl::connect(const RpcHost & host, boost::asio::io_service & ios, uint32_t conn_timeout,
                                 uint32_t read_timeout, RpcSessImplConnectCb client_connect_cb) {
    LOG(INFO) << "operation=connect host=" << host.getName() << ":" << host.getPort() << std::endl;

    auto self = shared_from_this();
    self->sess = std::make_shared<Nghttp2Session>(ios, host.getName(), host.getPort());

    boost::posix_time::time_duration td = boost::posix_time::seconds(conn_timeout);
    self->sess->connect_timeout(td);
    td = boost::posix_time::seconds(read_timeout);
    self->sess->read_timeout(td);

    ios.post(
        [self, host, client_connect_cb]() {

            self->sess->on_connect([host, client_connect_cb](tcp::resolver::iterator endpoint_it) {
                LOG(INFO) << "operation=sess_connect_cb host=" << endpoint_it->endpoint().address().to_string()
                          << ":" << endpoint_it->endpoint().port() << std::endl;

                client_connect_cb(host, RpcStatus::SUCCESS);
            });

            self->sess->on_error([self, host, client_connect_cb](const boost::system::error_code & ec) {
                LOG(ERROR) << "operation=sess_error_cb ec=" << ec.message() << " host=" << host.getName()
                           << ":" << host.getPort() << std::endl;
                client_connect_cb(host, self->getRpcStatus(ec));
            });
    });

    LOG(INFO) << "operation=connect status=success" << std::endl;
    return RpcStatus::SUCCESS;
}

RpcStatus RpcClientImpl::disconnect() {
    LOG(INFO) << "operation=disconnect" << std::endl;

    std::lock_guard<std::mutex> lg(_mutex);
    if (closed) {
        LOG(WARNING) << "operation=disconnect already closed" << std::endl;
        return RpcStatus::ALREADY_DISCONNECTED;
    }

    auto self = shared_from_this();
    auto & ios = self->sess->io_service();

    ios.post(
        [self] () {
        for (auto & req: self->req_map) {
           auto st = req.second;
           if (!st->getRequestImpl()->isClosed()) {
               st->getRequestImpl()->cancelRequest(req.first);
               st->getRequestImpl()->setClosed(true);
           }
        }
        self->req_map.clear();
    });
    closed = true;

    LOG(INFO) << "operation=disconnect status=success" << std::endl;
    return RpcStatus::SUCCESS;
}

void RpcClientImpl::onResponseCb(const Nghttp2ClientRes & res, const std::shared_ptr<Stream> & stream,
                                 RpcStatus res_code, RpcResImplReceiveCb res_cb) {
    LOG(INFO) << "operation=onResponseCb  res_code=" << static_cast<int>(res_code) << std::endl;

    std::lock_guard<std::mutex> lg(_mutex);
    if (closed) {
        LOG(INFO) << "operation=onResponseCb already closed" << std::endl;
        return;
    }
    if (stream->getRequestImpl()->isClosed()) { 
        LOG(INFO) << "operation=onResponseCb request already closed" << std::endl;
        return;
    }

    res.on_data([stream, res_code, res_cb] (const uint8_t * data, size_t len) {
        stream->getResponseImpl()->writePayload(data, len);
        if (len == 0) {
            res_cb(stream->getResponseImpl(), res_code);
        }
    });
}

void RpcClientImpl::onReqCloseCb(const std::shared_ptr<Stream> & stream, int req_id, int ec,
                                 RpcResImplReceiveCb res_cb) {
      LOG(INFO) << "operation=onReqCloseCb ec=" << ec << " , req_id=" << req_id << std::endl;

      std::lock_guard<std::mutex> lg(_mutex);
      if (closed) {
          LOG(INFO) << "operation=onReqCloseCb client is already closed" << std::endl;
          return;
      }

      if (stream->getRequestImpl()->isClosed()) {
        LOG(INFO) << "operation=onReqCloseCb request is already closed" << std::endl;
        return;
      }

      if (ec != 0) {
          res_cb(stream->getResponseImpl(), getRpcStatus(ec));
      }

      req_map.erase(req_id);
      LOG(INFO) << "operation=onReqCloseCb status=succes" << std::endl;
}

const Nghttp2ClientReq * RpcClientImpl::submitOnSession(const std::string & method,
                                         const std::string & url,
                                         const std::shared_ptr<Stream> & stream, generator_cb data_cb,
                                         boost::system::error_code & ec) {

    const header_map & hdr = stream->getRequestImpl()->readHeader();

    if (stream->getRequestImpl()->getPayloadType() == RpcHeader::BINARY_PAYLOAD) {
        auto req = sess->submit(ec, method, url, data_cb, hdr);
        return req;
    } else  {
        const std::string & data = stream->getRequestImpl()->readPayload();
        auto req = sess->submit(ec, method, url, data, hdr);
        return req;
    }

    return nullptr;
}

RpcStatus RpcClientImpl::submitRequest(const std::string & method, const std::string & url,
                                       const std::shared_ptr<Stream> & stream,
                                       RpcResImplReceiveCb res_cb) {
    LOG(INFO) << "operation=submitRequest url=" << url << ", method=" << method << std::endl;

    std::shared_ptr<size_t> offset = std::make_shared<size_t>(0);
    generator_cb data_cb =
        [stream, offset](uint8_t * buf, size_t len, uint32_t * flags) -> ssize_t {
            size_t ret = stream->getRequestImpl()->readPayload(*offset, len, buf);
            *offset += ret;
            if (ret < len) {
                *flags = NGHTTP2_DATA_FLAG_EOF;
            }
            DLOG(INFO) << "operation=generator_cb copied=" << ret << std::endl;
            return static_cast<ssize_t>(ret);
        };

    auto client_impl = shared_from_this();
    std::lock_guard<std::mutex> lg(_mutex);
    if (closed) {
        LOG(INFO) << "operation=submitRequest error=SessionAlreadyDisconnected " << std::endl;
        return RpcStatus::ALREADY_DISCONNECTED;
    }
    /* submit the request */
    boost::system::error_code ec;
    auto req = submitOnSession(method, url, stream, data_cb, ec);

    if (nullptr == req) {
        LOG(ERROR) << "operation=submitRequest ec=" << ec.value() << " , msg=" << ec.message() << std::endl;
        return getRpcStatus(ec);
    }

    /* store request information into map */
    uint64_t rid = ++client_impl->id;
    client_impl->req_map[rid] = stream;
    stream->getRequestImpl()->setRequest(req);
    /* on response call the response callback */
    req->on_response(
           [client_impl, stream, res_cb](const Nghttp2ClientRes & res) {
              client_impl->onResponseCb(res, stream, client_impl->getRpcStatus(res.status_code()), std::move(res_cb));
    });
    req->on_close(
           [client_impl, rid, stream, res_cb](uint32_t ec) {
            client_impl->onReqCloseCb(stream, rid, ec, std::move(res_cb));
    });

    return RpcStatus::SUCCESS;
}

RpcStatus RpcClientImpl::submitPOST(RpcRequestImpl & req_impl, RpcResImplReceiveCb res_cb) {
    LOG(INFO) << "operation=submitPOST buflen=" << req_impl.getPayload().getLength() << std::endl;

    std::string url = getUrl(req_impl);
    std::shared_ptr<Stream> stream = std::make_shared<Stream>(std::make_unique<RpcRequestImpl>(std::move(req_impl)),
                                                              std::make_unique<RpcResponseImpl>());

    auto self = shared_from_this();
    auto & ios = self->sess->io_service();

    ios.post(
       [self, url, stream, res_cb]() {
          RpcStatus ret = self->submitRequest("POST", url, stream, std::move(res_cb));
          if (ret != RpcStatus::SUCCESS) {
              LOG(ERROR) << "operation=submitPOST error=SubmitRequest ret=" << static_cast<uint32_t>(ret) << std::endl;
              res_cb(stream->getResponseImpl(), ret);
          }
    });

    return RpcStatus::SUCCESS;
}

RpcStatus RpcClientImpl::submitGET(RpcRequestImpl & req_impl, RpcResImplReceiveCb res_cb) {
    LOG(INFO) << "operation=submitGET buflen=" << req_impl.getPayload().getLength() << std::endl;

    std::string url = getUrl(req_impl);
    std::shared_ptr<Stream> stream = std::make_shared<Stream>(std::make_unique<RpcRequestImpl>(std::move(req_impl)),
                                                              std::make_unique<RpcResponseImpl>());

    auto self = shared_from_this();
    auto & ios = self->sess->io_service();

    ios.post(
       [self, url, stream, res_cb]() {
          RpcStatus ret = self->submitRequest("GET", url, stream, std::move(res_cb));
          if (ret != RpcStatus::SUCCESS) {
              LOG(ERROR) << "operation=submitGET error=submitRequest ret=" << static_cast<uint32_t>(ret) << std::endl;
              res_cb(stream->getResponseImpl(), ret);
          }
    });

    LOG(INFO) << "operation=submitGET status=success" << std::endl;
    return RpcStatus::SUCCESS;
}

uint32_t RpcClientImpl::numActiveRequests() const {
    return req_map.size();
}

} /* client */
} /* rpc */
