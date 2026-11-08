#ifndef TG_TRANSPORT_GRPCTRANSPORT_H_
#define TG_TRANSPORT_GRPCTRANSPORT_H_

// GrpcTransport (v5 §3.7): one shared Channel + credentials + auth metadata.
// Both streamer instances reuse this single Channel — do not create a second
// one per stream (v5 §4 step 4).
//
// Metadata (token) is attached at stream creation via MakeContext() — one
// ClientContext per stream, never reused, never per-message (v5 §3.4).

#include <app/Config.h>
#include <grpcpp/grpcpp.h>

#include <memory>

namespace tg::transport {

class GrpcTransport {
 public:
  // Creates the channel immediately (target must be configured).
  explicit GrpcTransport(const app::GrpcConfig& config);

  std::shared_ptr<grpc::Channel> channel() const { return channel_; }

  // Fresh ClientContext with auth metadata already applied.
  std::unique_ptr<grpc::ClientContext> MakeContext() const;

  const app::GrpcConfig& config() const { return config_; }

 private:
  app::GrpcConfig config_;
  std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace tg::transport

#endif  // TG_TRANSPORT_GRPCTRANSPORT_H_
