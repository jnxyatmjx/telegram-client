#include "transport/ProcessImagesClient.h"

#include <common/Logger.h>

namespace tg::transport {

ProcessImagesClient::ProcessImagesClient(GrpcTransport& transport)
    : stub_(transport.channel() != nullptr
                ? ImageProcessor::NewStub(transport.channel())
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
