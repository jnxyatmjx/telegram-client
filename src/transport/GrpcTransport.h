#ifndef TG_TRANSPORT_GRPCTRANSPORT_H_
#define TG_TRANSPORT_GRPCTRANSPORT_H_

// GrpcTransport (v5 §3.7): a pool of Channels + credentials + auth metadata.
// One lane = one Channel (one TCP/HTTP2 connection) to the same target.
// lane_count() is config.connections clamped to [1, 16]; 1 is the previous
// single-connection behavior. Both streamer pools reuse these Channels —
// never create a channel per stream outside this class (v5 §4 step 4).
//
// Metadata (token) is attached at stream creation via MakeContext() — one
// ClientContext per stream, never reused, never per-message (v5 §3.4).
//
// Channel args (Telethon migration chunk 3): every lane gets the same set
// from MakeChannelArgs — local subchannel pool (one TCP connection per
// lane), HTTP/2 keepalive, 64 MiB max message length (full images must fit,
// same headroom as the Python client's max_send_message_length) and the
// HTTP/2 lookahead / write-buffer sizing for multi-MB messages.

#include <app/Config.h>
#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace tg::transport {

// Channel args for one lane from the grpc config. Public + pure so the
// exact arg set is unit-testable without opening sockets (tests/
// test_grpc_channel_args.cpp). Always sets GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL;
// keepalive / message-length / window args are set when their config value
// is > 0 (0 = keep gRPC's default).
grpc::ChannelArguments MakeChannelArgs(const app::GrpcConfig& config);

class GrpcTransport {
 public:
  // Creates the channels immediately (target must be configured).
  explicit GrpcTransport(const app::GrpcConfig& config);

  // Lane 0, kept for single-lane callers.
  std::shared_ptr<grpc::Channel> channel() const {
    return channels_.empty() ? nullptr : channels_.front();
  }
  // Channel for lane `lane` (nullptr when out of range or no target).
  std::shared_ptr<grpc::Channel> channel(std::size_t lane) const {
    return lane < channels_.size() ? channels_[lane] : nullptr;
  }
  std::size_t lane_count() const { return channels_.size(); }

  // Fresh ClientContext with auth metadata already applied.
  std::unique_ptr<grpc::ClientContext> MakeContext() const;

  const app::GrpcConfig& config() const { return config_; }

 private:
  // Upper bound: each lane adds 2 CQ threads + 2 stubs; beyond this the
  // extra parallelism buys nothing for thumbnail traffic.
  static constexpr std::size_t kMaxLanes = 16;

  app::GrpcConfig config_;
  std::vector<std::shared_ptr<grpc::Channel>> channels_;
};

}  // namespace tg::transport

#endif  // TG_TRANSPORT_GRPCTRANSPORT_H_
