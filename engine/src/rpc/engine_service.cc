#include "engine_service.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vyuha::rpc {

namespace pb = vyuha::engine::v1;
namespace common = vyuha::common::v1;

namespace {

// Overwrites a secret in the request buffer once it's been copied into a SecretRef. The request
// is owned by gRPC and only handed to us as const; it isn't a const object, so writing is defined.
void wipe(const std::string& s) {
  auto& m = const_cast<std::string&>(s);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  std::fill(m.begin(), m.end(), '\0');
}

grpc::Status to_status(const Error& e) {
  switch (e.code) {
    case ErrorCode::kStaticIpNotOnHost:
      return {grpc::StatusCode::FAILED_PRECONDITION, "STATIC_IP_NOT_ON_HOST"};
    case ErrorCode::kCapacityExceeded:
      return {grpc::StatusCode::RESOURCE_EXHAUSTED, std::string(e.detail)};
    case ErrorCode::kConnectFailed:
      return {grpc::StatusCode::UNAVAILABLE, std::string(e.detail)};
    default:
      return {grpc::StatusCode::INVALID_ARGUMENT, std::string(e.detail)};
  }
}

common::Broker to_pb(session::BrokerKind b) {
  switch (b) {
    case session::BrokerKind::kZerodha:
      return common::BROKER_ZERODHA;
    case session::BrokerKind::kUpstox:
      return common::BROKER_UPSTOX;
    case session::BrokerKind::kSim:
      return common::BROKER_SIM;
  }
  return common::BROKER_UNSPECIFIED;
}

vyuha::marketdata::v1::FeedStatus::State feed_state(const session::SessionStatus& s) {
  if (s.subscribed == 0) {
    return vyuha::marketdata::v1::FeedStatus::STATE_UNSPECIFIED;
  }
  return s.feed_connects == 0 ? vyuha::marketdata::v1::FeedStatus::STATE_CONNECTING
                              : vyuha::marketdata::v1::FeedStatus::STATE_CONNECTED;
}

void fill(const session::SessionStatus& s, pb::SessionInfo& out) {
  out.set_account_id(s.account_id);
  out.set_broker(to_pb(s.broker));
  out.set_state(std::string(session::to_string(s.state)));
  out.set_detail(s.detail);
  out.set_ops_cap(s.ops_cap);
  out.set_feed_id(s.feed_id);
  if (s.data_entitled && s.broker != session::BrokerKind::kSim) {
    auto* f = out.add_feeds();
    f->set_account_id(s.account_id);
    f->set_broker(to_pb(s.broker));
    f->set_subscribed(static_cast<std::uint32_t>(s.subscribed));
    f->set_state(feed_state(s));
    f->set_detail("restarts=" + std::to_string(s.feed_restarts) + " gaps=" + std::to_string(s.feed_gaps) +
                  " unresolved=" + std::to_string(s.unresolved));
  }
}

session::FeedMode to_mode(vyuha::marketdata::v1::FeedMode m) {
  switch (m) {
    case vyuha::marketdata::v1::FEED_MODE_LTP:
      return session::FeedMode::kLtp;
    case vyuha::marketdata::v1::FEED_MODE_FULL:
      return session::FeedMode::kFull;
    default:
      return session::FeedMode::kQuote;
  }
}

}  // namespace

grpc::Status EngineServiceImpl::StartSession(grpc::ServerContext* /*ctx*/, const pb::StartSessionRequest* req,
                                             pb::SessionInfo* out) {
  session::SessionRequest r;
  r.account_id = req->account_id();
  switch (req->broker()) {
    case common::BROKER_ZERODHA:
      r.broker = session::BrokerKind::kZerodha;
      break;
    case common::BROKER_UPSTOX:
      r.broker = session::BrokerKind::kUpstox;
      break;
    case common::BROKER_SIM:
      r.broker = session::BrokerKind::kSim;
      break;
    default:
      return {grpc::StatusCode::INVALID_ARGUMENT, "broker required"};
  }
  r.api_key = req->api_key();
  r.api_secret = SecretRef(req->api_secret());
  r.access_token = SecretRef(req->access_token());
  wipe(req->api_secret());
  wipe(req->access_token());
  r.source_ip = req->source_ip();
  r.backup_source_ip = req->backup_source_ip();
  r.public_ip = req->public_ip();
  r.ops_cap = req->ops_cap() == 0 ? 8 : req->ops_cap();
  if (r.ops_cap > 10) {  // invariant 7: never at or above the 10 OPS threshold without a DECISIONS change
    return {grpc::StatusCode::INVALID_ARGUMENT, "ops_cap must be 1..10"};
  }
  r.data_entitled = req->data_entitled();
  const std::string account = r.account_id;
  auto st = sessions_->start(std::move(r));
  if (!st) {
    // The refused session is still listed (state=error) so the status bar shows why.
    for (const auto& s : sessions_->sessions()) {
      if (s.account_id == account) {
        fill(s, *out);
      }
    }
    return to_status(st.error());
  }
  fill(*st, *out);
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::StopSession(grpc::ServerContext* /*ctx*/, const pb::StopSessionRequest* req,
                                            pb::Ack* out) {
  const auto r = sessions_->stop(req->account_id());
  out->set_ok(r.has_value());
  if (!r) {
    out->set_code("SESSION_UNKNOWN");
    out->set_message(std::string(r.error().detail));
  }
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::GetSessions(grpc::ServerContext* /*ctx*/,
                                            const pb::GetSessionsRequest* /*req*/, pb::SessionList* out) {
  for (const auto& s : sessions_->sessions()) {
    fill(s, *out->add_sessions());
  }
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::Subscribe(grpc::ServerContext* /*ctx*/, const pb::SubscribeRequest* req,
                                          pb::Ack* out) {
  const std::vector<md::InstrumentId> ids(req->instrument_ids().begin(), req->instrument_ids().end());
  const auto r = sessions_->subscribe(req->requester(), ids, to_mode(req->mode()), req->via_account_id());
  out->set_ok(r.has_value());
  if (!r) {
    out->set_code("SUBSCRIBE_REJECTED");
    out->set_message(std::string(r.error().detail));
  }
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::Unsubscribe(grpc::ServerContext* /*ctx*/, const pb::SubscribeRequest* req,
                                            pb::Ack* out) {
  const std::vector<md::InstrumentId> ids(req->instrument_ids().begin(), req->instrument_ids().end());
  const auto r = sessions_->unsubscribe(req->requester(), ids, req->via_account_id());
  out->set_ok(r.has_value());
  if (!r) {
    out->set_code("UNSUBSCRIBE_REJECTED");
    out->set_message(std::string(r.error().detail));
  }
  return grpc::Status::OK;
}

grpc::Status EngineServiceImpl::Health(grpc::ServerContext* /*ctx*/, const pb::HealthRequest* /*req*/,
                                       common::Health* out) {
  out->set_service("engine");
  out->set_ok(true);
  std::size_t errors = 0;
  for (const auto& s : sessions_->sessions()) {
    errors += s.state == session::SessionState::kError ? 1 : 0;
  }
  (*out->mutable_checks())["sessions_in_error"] = std::to_string(errors);
  return grpc::Status::OK;
}

}  // namespace vyuha::rpc
