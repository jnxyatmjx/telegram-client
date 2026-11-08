#ifndef TG_TRANSPORT_SEARCHIMAGESCLIENT_H_
#define TG_TRANSPORT_SEARCHIMAGESCLIENT_H_

// SearchImagesClient (v5 §3.7): the ImageProcessor::Stub's SearchImages half.
// Reply handling ((oid,id) correlation lookup) is wired by Application.

#include <transport/AsyncStreamer.h>
#include <transport/GrpcTransport.h>

#include <memory>

#include "image_stream.grpc.pb.h"

namespace tg::transport {

class SearchImagesClient {
 public:
  // Stub on the transport's lane-0 channel.
  explicit SearchImagesClient(GrpcTransport& transport);
  // Stub on the transport's channel for `lane`.
  SearchImagesClient(GrpcTransport& transport, std::size_t lane);

  AsyncStreamer<SearchReq, SearchRsp>::StartFn start_fn() const;

 private:
  std::unique_ptr<ImageProcessor::Stub> stub_;
  GrpcTransport& transport_;
};

using SearchImagesStreamer = AsyncStreamer<SearchReq, SearchRsp>;

}  // namespace tg::transport

#endif  // TG_TRANSPORT_SEARCHIMAGESCLIENT_H_
