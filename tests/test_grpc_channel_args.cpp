// GrpcTransport channel args (Telethon migration chunk 3): the exact arg
// set each lane's Channel gets, from MakeChannelArgs — keepalive, the 64 MiB
// message cap (full images, same headroom as the Python client's
// max_send_message_length) and HTTP/2 lookahead / write buffer. Pure
// function, no sockets needed.
#include <transport/GrpcTransport.h>

#include <app/Config.h>

#include <grpcpp/support/channel_arguments.h>

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>

namespace {

using tg::app::GrpcConfig;
using tg::transport::MakeChannelArgs;

constexpr std::int64_t kMiB = 1024 * 1024;

// gRPC 1.51's ChannelArguments exposes the raw list via c_channel_args()
// (no GetInt): find one arg by key. The returned pointer borrows from
// `args` — valid while it lives.
const grpc_arg* FindArg(const grpc::ChannelArguments& args,
                        const char* key) {
  const grpc_channel_args c_args = args.c_channel_args();
  for (int i = 0; i < c_args.num_args; ++i) {
    if (c_args.args[i].key != nullptr &&
        std::strcmp(c_args.args[i].key, key) == 0) {
      return &c_args.args[i];
    }
  }
  return nullptr;
}

std::optional<int> IntArg(const grpc::ChannelArguments& args,
                          const char* key) {
  const grpc_arg* arg = FindArg(args, key);
  if (arg == nullptr || arg->type != GRPC_ARG_INTEGER) return std::nullopt;
  return arg->value.integer;
}

TEST(GrpcChannelArgs, DefaultsCarryKeepaliveMessageCapAndWindow) {
  GrpcConfig config;  // library defaults
  const grpc::ChannelArguments args = MakeChannelArgs(config);

  // Per-lane local subchannel pool (one TCP connection per lane) — set since
  // the lanes feature, unchanged by chunk 3.
  EXPECT_EQ(IntArg(args, GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL),
            std::optional<int>(1));

  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_TIME_MS),
            std::optional<int>(60000));
  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_TIMEOUT_MS),
            std::optional<int>(20000));
  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS),
            std::optional<int>(1));

  // 64 MiB both directions: chunk 1 sends the FULL image in one
  // ImageRequest; the Python client sets its sender cap to 64 MiB for the
  // same reason.
  EXPECT_EQ(IntArg(args, GRPC_ARG_MAX_SEND_MESSAGE_LENGTH),
            std::optional<int>(static_cast<int>(64 * kMiB)));
  EXPECT_EQ(IntArg(args, GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH),
            std::optional<int>(static_cast<int>(64 * kMiB)));

  EXPECT_EQ(IntArg(args, GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES),
            std::optional<int>(static_cast<int>(1 * kMiB)));
  EXPECT_EQ(IntArg(args, GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE),
            std::optional<int>(static_cast<int>(512 * 1024)));
}

TEST(GrpcChannelArgs, ZeroKeepsGrpcDefaultsForThatArg) {
  GrpcConfig config;
  config.keepalive_ms = 0;
  config.keepalive_timeout_ms = 0;
  config.max_message_bytes = 0;
  config.http2_lookahead_bytes = 0;
  config.http2_write_buffer_bytes = 0;
  const grpc::ChannelArguments args = MakeChannelArgs(config);

  EXPECT_EQ(FindArg(args, GRPC_ARG_KEEPALIVE_TIME_MS), nullptr);
  EXPECT_EQ(FindArg(args, GRPC_ARG_KEEPALIVE_TIMEOUT_MS), nullptr);
  EXPECT_EQ(FindArg(args, GRPC_ARG_MAX_SEND_MESSAGE_LENGTH), nullptr);
  EXPECT_EQ(FindArg(args, GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH), nullptr);
  EXPECT_EQ(FindArg(args, GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES), nullptr);
  EXPECT_EQ(FindArg(args, GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE), nullptr);
  // The lane pool flag is not optional.
  EXPECT_NE(FindArg(args, GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL), nullptr);
}

TEST(GrpcChannelArgs, ConfigValuesReachTheArgs) {
  GrpcConfig config;
  config.keepalive_ms = 5000;
  config.keepalive_timeout_ms = 1500;
  config.keepalive_permit_without_calls = false;
  config.max_message_bytes = 8 * kMiB;
  config.http2_lookahead_bytes = 256 * 1024;
  config.http2_write_buffer_bytes = 64 * 1024;
  const grpc::ChannelArguments args = MakeChannelArgs(config);

  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_TIME_MS), std::optional<int>(5000));
  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_TIMEOUT_MS),
            std::optional<int>(1500));
  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS),
            std::optional<int>(0));
  EXPECT_EQ(IntArg(args, GRPC_ARG_MAX_SEND_MESSAGE_LENGTH),
            std::optional<int>(static_cast<int>(8 * kMiB)));
  EXPECT_EQ(IntArg(args, GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH),
            std::optional<int>(static_cast<int>(8 * kMiB)));
  EXPECT_EQ(IntArg(args, GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES),
            std::optional<int>(static_cast<int>(256 * 1024)));
  EXPECT_EQ(IntArg(args, GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE),
            std::optional<int>(static_cast<int>(64 * 1024)));
}

TEST(GrpcChannelArgs, KeepalivePermitWithoutCallsIsDisablable) {
  GrpcConfig config;
  config.keepalive_permit_without_calls = false;
  const grpc::ChannelArguments args = MakeChannelArgs(config);
  EXPECT_EQ(IntArg(args, GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS),
            std::optional<int>(0));
}

}  // namespace
