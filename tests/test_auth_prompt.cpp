// Interactive login prompts (td_example.cpp style): the login code and the
// 2FA password are read from the terminal, with TG_TELEGRAM_CODE /
// TG_TELEGRAM_PASSWORD as an environment override. The prompt provider takes
// its input stream as a parameter, so tests inject a string stream instead
// of the real stdin.
#include <app/Config.h>
#include <telegram/AuthManager.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>
#include <string>

namespace td_api = td::td_api;  // shorthand for td::td_api::*

namespace {

// Minimal fake TDLib: records the auth request it receives and answers ok.
class FakeAuthSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    last_type_ = request->get_id();
    if (last_type_ == td_api::checkAuthenticationCode::ID) {
      auto code_req =
          td_api::move_object_as<td_api::checkAuthenticationCode>(std::move(request));
      last_value_ = code_req->code_;
    } else if (last_type_ == td_api::checkAuthenticationPassword::ID) {
      auto pw_req = td_api::move_object_as<td_api::checkAuthenticationPassword>(
          std::move(request));
      last_value_ = pw_req->password_;
    }
    td::ClientManager::Response response;
    response.request_id = 1;
    response.object = td_api::make_object<td_api::ok>();
    callback(std::move(response));
    return 1;
  }

  std::int32_t last_type_ = 0;
  std::string last_value_;
};

tg::telegram::AuthManager MakeAuth(FakeAuthSender& sender) {
  tg::app::TelegramConfig config;  // unused by the code/password states
  return tg::telegram::AuthManager(sender, config);
}

}  // namespace

TEST(AuthPrompt, TerminalPromptProvidesCode) {
  ::unsetenv("TG_TELEGRAM_CODE");
  FakeAuthSender sender;
  auto auth = MakeAuth(sender);
  std::istringstream in("12345\n");
  auth.SetCodeProvider([&] {
    return tg::telegram::PromptTerminal("Enter authentication code: ", in, false);
  });
  auth.HandleAuthorizationState(
      td_api::make_object<td_api::authorizationStateWaitCode>());
  EXPECT_EQ(sender.last_type_, td_api::checkAuthenticationCode::ID);
  EXPECT_EQ(sender.last_value_, "12345");
  EXPECT_FALSE(auth.HasFailed());
}

TEST(AuthPrompt, TerminalPromptProvidesPasswordWithSpaces) {
  ::unsetenv("TG_TELEGRAM_PASSWORD");
  FakeAuthSender sender;
  auto auth = MakeAuth(sender);
  std::istringstream in("my secret pw\n");
  auth.SetPasswordProvider([&] {
    return tg::telegram::PromptTerminal("Enter authentication password: ", in,
                                        true);
  });
  auth.HandleAuthorizationState(
      td_api::make_object<td_api::authorizationStateWaitPassword>());
  EXPECT_EQ(sender.last_type_, td_api::checkAuthenticationPassword::ID);
  EXPECT_EQ(sender.last_value_, "my secret pw");
  EXPECT_FALSE(auth.HasFailed());
}

TEST(AuthPrompt, CodeThenPasswordPromptReadsBoth) {
  // The code is read with operator>>, which leaves the newline behind; the
  // password prompt (getline) must not read that newline as an empty
  // password. Exercises the full waitCode -> waitPassword sequence.
  ::unsetenv("TG_TELEGRAM_CODE");
  ::unsetenv("TG_TELEGRAM_PASSWORD");
  FakeAuthSender sender;
  auto auth = MakeAuth(sender);
  std::istringstream in("12345\nmy secret pw\n");
  auth.SetCodeProvider([&] {
    return tg::telegram::PromptTerminal("Enter authentication code: ", in, false);
  });
  auth.SetPasswordProvider([&] {
    return tg::telegram::PromptTerminal("Enter authentication password: ", in,
                                        true);
  });
  auth.HandleAuthorizationState(
      td_api::make_object<td_api::authorizationStateWaitCode>());
  EXPECT_EQ(sender.last_value_, "12345");
  auth.HandleAuthorizationState(
      td_api::make_object<td_api::authorizationStateWaitPassword>());
  EXPECT_EQ(sender.last_type_, td_api::checkAuthenticationPassword::ID);
  EXPECT_EQ(sender.last_value_, "my secret pw");
  EXPECT_FALSE(auth.HasFailed());
}

TEST(AuthPrompt, EnvVarOverridesTerminalPrompt) {
  const char* old = ::getenv("TG_TELEGRAM_CODE");
  const std::string saved = old != nullptr ? old : std::string{};
  ::setenv("TG_TELEGRAM_CODE", "99999", 1);
  FakeAuthSender sender;
  auto auth = MakeAuth(sender);  // default provider: env first, no stdin read
  auth.HandleAuthorizationState(
      td_api::make_object<td_api::authorizationStateWaitCode>());
  EXPECT_EQ(sender.last_type_, td_api::checkAuthenticationCode::ID);
  EXPECT_EQ(sender.last_value_, "99999");
  EXPECT_FALSE(auth.HasFailed());
  if (!saved.empty()) {
    ::setenv("TG_TELEGRAM_CODE", saved.c_str(), 1);
  } else {
    ::unsetenv("TG_TELEGRAM_CODE");
  }
}

TEST(AuthPrompt, EmptyTerminalInputFailsFast) {
  ::unsetenv("TG_TELEGRAM_CODE");
  FakeAuthSender sender;
  auto auth = MakeAuth(sender);
  std::istringstream in("");  // immediate EOF, as with a closed stdin
  auth.SetCodeProvider([&] {
    return tg::telegram::PromptTerminal("Enter authentication code: ", in, false);
  });
  auth.HandleAuthorizationState(
      td_api::make_object<td_api::authorizationStateWaitCode>());
  EXPECT_EQ(sender.last_type_, 0);  // nothing was sent
  EXPECT_TRUE(auth.HasFailed());
}
