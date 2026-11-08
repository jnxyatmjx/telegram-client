#include "transport/SearchImagesClient.h"

#include <common/Logger.h>

namespace tg::transport {

SearchImagesClient::SearchImagesClient(GrpcTransport& transport)
    : SearchImagesClient(transport, 0) {}

SearchImagesClient::SearchImagesClient(GrpcTransport& transport,
                                       std::size_t lane)
    : stub_(transport.channel(lane) != nullptr
                ? ImageProcessor::NewStub(transport.channel(lane))
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
