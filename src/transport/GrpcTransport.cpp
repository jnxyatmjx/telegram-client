#include "transport/GrpcTransport.h"

#include <common/Logger.h>

namespace tg::transport {

GrpcTransport::GrpcTransport(const app::GrpcConfig& config)
    : config_(config) {
  if (config_.target.empty()) {
    // Legal for skeleton runs (e.g. unit tests); streams simply won't start
    // against a real server.
    spdlog::warn("grpc: no target configured — channel will not be created");
    return;
  }
  std::shared_ptr<grpc::ChannelCredentials> credentials =
      config_.tls ? grpc::SslCredentials(grpc::SslCredentialsOptions())
                  : grpc::InsecureChannelCredentials();
  channel_ = grpc::CreateChannel(config_.target, credentials);
  spdlog::info("grpc: channel created for {} ({})", config_.target,
               config_.tls ? "TLS" : "insecure");
}

std::unique_ptr<grpc::ClientContext> GrpcTransport::MakeContext() const {
  auto context = std::make_unique<grpc::ClientContext>();
  if (!config_.token.empty()) {
    // Authorization header once per stream, not per message (v5 §3.4).
    context->AddMetadata("authorization", "Bearer " + config_.token);
  }
  return context;
}

}  // namespace tg::transport
