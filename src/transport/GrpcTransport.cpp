#include "transport/GrpcTransport.h"

#include <common/Logger.h>

#include <infra/FileStore.h>

#include <stdexcept>

namespace tg::transport {

GrpcTransport::GrpcTransport(const app::GrpcConfig& config)
    : config_(config) {
  if (config_.target.empty()) {
    // Legal for skeleton runs (e.g. unit tests); streams simply won't start
    // against a real server.
    spdlog::warn("grpc: no target configured — channels will not be created");
    return;
  }
  std::shared_ptr<grpc::ChannelCredentials> credentials;
  if (!config_.tls) {
    credentials = grpc::InsecureChannelCredentials();
  } else if (!config_.ca_cert_path.empty()) {
    // Custom CA (e.g. a private/self-signed CA): read the PEM bundle and
    // hand it to gRPC as the root certs. An unreadable file fails loudly
    // here at startup instead of surfacing as a confusing verify error on
    // the first connect.
    auto pem = infra::ReadFileBytes(config_.ca_cert_path, 1 << 20);
    if (!pem) {
      throw std::runtime_error("grpc: cannot read CA cert file " +
                               config_.ca_cert_path);
    }
    grpc::SslCredentialsOptions options;
    options.pem_root_certs = *pem;
    credentials = grpc::SslCredentials(options);
    spdlog::info("grpc: using custom CA certs from {}", config_.ca_cert_path);
  } else {
    credentials = grpc::SslCredentials(grpc::SslCredentialsOptions());
  }
  // One Channel per lane: each is its own TCP/HTTP2 connection to the same
  // target, so traffic actually spreads across connections (a single
  // channel multiplexes streams but stays on one connection).
  std::size_t lanes = static_cast<std::size_t>(
      config_.connections > 0 ? config_.connections : 1);
  if (lanes > kMaxLanes) lanes = kMaxLanes;
  channels_.reserve(lanes);
  for (std::size_t i = 0; i < lanes; ++i) {
    channels_.push_back(grpc::CreateChannel(config_.target, credentials));
  }
  spdlog::info("grpc: {} channel(s) created for {} ({})", channels_.size(),
               config_.target, config_.tls ? "TLS" : "insecure");
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
