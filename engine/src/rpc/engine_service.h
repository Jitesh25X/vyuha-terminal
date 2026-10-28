#pragma once
// gRPC adapter for engine.v1.EngineService. Internal: exposes generated protobuf/gRPC types.
// Compiled without sanitizers (vcpkg's gRPC/abseil aren't instrumented); all logic lives in the
// sanitizer-tested SessionManager.

#include <grpcpp/grpcpp.h>

#include "vyuha/engine/v1/engine.grpc.pb.h"
#include "vyuha/session/session_manager.h"

namespace vyuha::rpc {

class EngineServiceImpl final : public vyuha::engine::v1::EngineService::Service {
 public:
  explicit EngineServiceImpl(session::SessionManager& sessions) : sessions_(&sessions) {}

  grpc::Status StartSession(grpc::ServerContext* ctx, const vyuha::engine::v1::StartSessionRequest* req,
                            vyuha::engine::v1::SessionInfo* out) override;
  grpc::Status StopSession(grpc::ServerContext* ctx, const vyuha::engine::v1::StopSessionRequest* req,
                           vyuha::engine::v1::Ack* out) override;
  grpc::Status GetSessions(grpc::ServerContext* ctx, const vyuha::engine::v1::GetSessionsRequest* req,
                           vyuha::engine::v1::SessionList* out) override;
  grpc::Status Subscribe(grpc::ServerContext* ctx, const vyuha::engine::v1::SubscribeRequest* req,
                         vyuha::engine::v1::Ack* out) override;
  grpc::Status Unsubscribe(grpc::ServerContext* ctx, const vyuha::engine::v1::SubscribeRequest* req,
                           vyuha::engine::v1::Ack* out) override;
  grpc::Status Health(grpc::ServerContext* ctx, const vyuha::engine::v1::HealthRequest* req,
                      vyuha::common::v1::Health* out) override;

 private:
  session::SessionManager* sessions_;  // not owned; outlives the service
};

}  // namespace vyuha::rpc
