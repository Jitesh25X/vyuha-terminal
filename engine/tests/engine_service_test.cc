// gRPC EngineService end to end, in process: StartSession over the wire refuses a static IP that
// isn't on the host (F-EX-3 AC1 / M1.6 done check) and the refusal is visible in GetSessions.

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "../src/rpc/engine_service.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"

namespace {

namespace pb = vyuha::engine::v1;

struct Server {
  vyuha::SimClock clock{1};
  vyuha::session::StaticInstrumentDirectory dir{{}};
  std::unique_ptr<vyuha::TickRing> ring = std::make_unique<vyuha::TickRing>();
  std::vector<vyuha::TickRing*> rings{ring.get()};
  std::unique_ptr<vyuha::session::SessionManager> sessions;
  std::unique_ptr<vyuha::rpc::EngineServiceImpl> service;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<pb::EngineService::Stub> stub;

  Server() {
    vyuha::session::SessionManagerConfig cfg;
    cfg.background_apply = false;
    sessions = std::make_unique<vyuha::session::SessionManager>(cfg, dir, rings, clock);
    service = std::make_unique<vyuha::rpc::EngineServiceImpl>(*sessions);
    grpc::ServerBuilder b;
    int port = 0;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(service.get());
    server = b.BuildAndStart();
    stub = pb::EngineService::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&&) = delete;
  Server& operator=(Server&&) = delete;
  ~Server() { server->Shutdown(); }
};

pb::StartSessionRequest request(const std::string& account, const std::string& ip) {
  pb::StartSessionRequest r;
  r.set_account_id(account);
  r.set_broker(vyuha::common::v1::BROKER_ZERODHA);
  r.set_api_key("key");
  r.set_api_secret("secret");
  r.set_access_token("token");
  r.set_source_ip(ip);
  r.set_data_entitled(true);
  return r;
}

TEST(EngineService, StartSessionRefusesStaticIpNotOnHost) {
  Server s;
  grpc::ClientContext ctx;
  pb::SessionInfo info;
  const grpc::Status st = s.stub->StartSession(&ctx, request("acct-z1", "203.0.113.7"), &info);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(st.error_message(), "STATIC_IP_NOT_ON_HOST");

  grpc::ClientContext ctx2;
  pb::SessionList list;
  ASSERT_TRUE(s.stub->GetSessions(&ctx2, pb::GetSessionsRequest{}, &list).ok());
  ASSERT_EQ(list.sessions_size(), 1);
  EXPECT_EQ(list.sessions(0).state(), "error");
  EXPECT_EQ(list.sessions(0).detail(), "STATIC_IP_NOT_ON_HOST");
}

TEST(EngineService, StartSubscribeStopAndOpsCapGuard) {
  Server s;
  {
    grpc::ClientContext ctx;
    pb::SessionInfo info;
    ASSERT_TRUE(s.stub->StartSession(&ctx, request("acct-z1", "127.0.0.1"), &info).ok());
    EXPECT_EQ(info.state(), "connected");
    EXPECT_EQ(info.ops_cap(), 8U);  // invariant 7 default
  }
  {
    grpc::ClientContext ctx;
    pb::SubscribeRequest sub;
    sub.set_requester("gateway");
    sub.add_instrument_ids(1);
    sub.set_mode(vyuha::marketdata::v1::FEED_MODE_QUOTE);
    pb::Ack ack;
    ASSERT_TRUE(s.stub->Subscribe(&ctx, sub, &ack).ok());
    EXPECT_TRUE(ack.ok());
  }
  {
    grpc::ClientContext ctx;
    auto r = request("acct-z2", "127.0.0.1");
    r.set_ops_cap(11);
    pb::SessionInfo info;
    EXPECT_EQ(s.stub->StartSession(&ctx, r, &info).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    grpc::ClientContext ctx;
    pb::StopSessionRequest stop;
    stop.set_account_id("acct-z1");
    pb::Ack ack;
    ASSERT_TRUE(s.stub->StopSession(&ctx, stop, &ack).ok());
    EXPECT_TRUE(ack.ok());
  }
  grpc::ClientContext ctx;
  vyuha::common::v1::Health h;
  ASSERT_TRUE(s.stub->Health(&ctx, pb::HealthRequest{}, &h).ok());
  EXPECT_EQ(h.service(), "engine");
}

}  // namespace
