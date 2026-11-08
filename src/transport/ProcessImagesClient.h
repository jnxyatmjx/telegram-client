#ifndef TG_TRANSPORT_PROCESSIMAGESCLIENT_H_
#define TG_TRANSPORT_PROCESSIMAGESCLIENT_H_

// ProcessImagesClient (v5 §3.7): thin owner of the ImageProcessor::Stub's
// ProcessImages half. Produces the AsyncStreamer StartFn; reply handling
// (Outbox MarkDone by image_id) is wired by Application.

#include <transport/AsyncStreamer.h>
#include <transport/GrpcTransport.h>

#include <memory>

#include "image_stream.grpc.pb.h"

namespace tg::transport {

class ProcessImagesClient {
 public:
  explicit ProcessImagesClient(GrpcTransport& transport);

  AsyncStreamer<ImageRequest, ImageReply>::StartFn start_fn() const;

 private:
  // Shared stub; ProcessImagesClient and SearchImagesClient each own their
  // own stub off the same channel (a stub is cheap, per-service).
  std::unique_ptr<ImageProcessor::Stub> stub_;
  GrpcTransport& transport_;
};

using ProcessImagesStreamer = AsyncStreamer<ImageRequest, ImageReply>;

}  // namespace tg::transport

#endif  // TG_TRANSPORT_PROCESSIMAGESCLIENT_H_
