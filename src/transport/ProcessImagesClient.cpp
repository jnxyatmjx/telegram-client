#include "transport/ProcessImagesClient.h"

#include <common/Logger.h>

namespace tg::transport {

ProcessImagesClient::ProcessImagesClient(GrpcTransport& transport)
    : ProcessImagesClient(transport, 0) {}

ProcessImagesClient::ProcessImagesClient(GrpcTransport& transport,
                                         std::size_t lane)
    : stub_(transport.channel(lane) != nullptr
                ? ImageProcessor::NewStub(transport.channel(lane))
                : nullptr),
      transport_(transport) {}

AsyncStreamer<ImageRequest, ImageReply>::StartFn
ProcessImagesClient::start_fn() const {
  ImageProcessor::StubInterface* stub = stub_.get();
  if (stub == nullptr) {
    return nullptr;
  }
  return [stub](grpc::ClientContext* context, grpc::CompletionQueue* cq) {
    return stub->PrepareAsyncProcessImages(context, cq);
  };
}

}  // namespace tg::transport
