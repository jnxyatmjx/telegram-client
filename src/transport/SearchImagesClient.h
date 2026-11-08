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
  explicit SearchImagesClient(GrpcTransport& transport);

  AsyncStreamer<SearchReq, SearchRsp>::StartFn start_fn() const;

 private:
  std::unique_ptr<ImageProcessor::Stub> stub_;
  GrpcTransport& transport_;
};

using SearchImagesStreamer = AsyncStreamer<SearchReq, SearchRsp>;

}  // namespace tg::transport

#endif  // TG_TRANSPORT_SEARCHIMAGESCLIENT_H_
