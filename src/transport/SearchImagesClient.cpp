#include "transport/SearchImagesClient.h"

#include <common/Logger.h>

namespace tg::transport {

SearchImagesClient::SearchImagesClient(GrpcTransport& transport)
    : stub_(transport.channel() != nullptr
                ? ImageProcessor::NewStub(transport.channel())
                : nullptr),
      transport_(transport) {}

AsyncStreamer<SearchReq, SearchRsp>::StartFn SearchImagesClient::start_fn()
    const {
  ImageProcessor::StubInterface* stub = stub_.get();
  if (stub == nullptr) {
    return nullptr;
  }
  return [stub](grpc::ClientContext* context, grpc::CompletionQueue* cq) {
    return stub->PrepareAsyncSearchImages(context, cq);
  };
}

}  // namespace tg::transport
