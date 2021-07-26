// Concord
//
// Copyright (c) 2021 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the "License"). You may not use this product except in
// compliance with the Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright notices and license terms. Your use of
// these subcomponents is subject to the terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#include <chrono>
#include <iostream>
#include <google/protobuf/util/time_util.h>
#include <opentracing/tracer.h>
#include <thread>

#include "client/clientservice/event_service.hpp"
#include "client/concordclient/concord_client.hpp"

using grpc::Status;
using grpc::ServerContext;
using grpc::ServerWriter;

using google::protobuf::util::TimeUtil;

using vmware::concord::client::v1::SubscribeRequest;
using vmware::concord::client::v1::SubscribeResponse;
using vmware::concord::client::v1::EventGroup;

using namespace std::chrono_literals;

namespace cc = concord::client::concordclient;

namespace concord::client::clientservice {

Status EventServiceImpl::Subscribe(ServerContext* context,
                                   const SubscribeRequest* proto_request,
                                   ServerWriter<SubscribeResponse>* stream) {
  if (proto_request->has_events()) {
    LOG_INFO(logger_, "Legacy events not supported yet");
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "Legacy events");
  }
  ConcordAssert(proto_request->has_event_groups());

  cc::SubscribeRequest request;
  cc::EventGroupRequest eg_request;
  eg_request.event_group_id = proto_request->event_groups().event_group_id();
  request.request = eg_request;

  auto callback = [this, stream](cc::SubscribeResult&& subscribe_result) {
    if (not std::holds_alternative<cc::EventGroup>(subscribe_result)) {
      LOG_INFO(logger_, "Subscribe returned error");
      return;
    }
    auto event_group = std::get<cc::EventGroup>(subscribe_result);
    EventGroup proto_event_group;
    proto_event_group.set_id(event_group.id);
    for (cc::Event e : event_group.events) {
      *proto_event_group.add_events() = std::string(e.begin(), e.end());
    }
    *proto_event_group.mutable_record_time() = TimeUtil::MicrosecondsToTimestamp(event_group.record_time.count());
    *proto_event_group.mutable_trace_context() = {event_group.trace_context.begin(), event_group.trace_context.end()};

    SubscribeResponse response;
    *response.mutable_event_group() = proto_event_group;

    stream->Write(response);
  };

  auto span = opentracing::Tracer::Global()->StartSpan("subscribe", {});
  try {
    client_->subscribe(request, span, callback);
  } catch (cc::ConcordClient::SubscriptionExists& e) {
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
  }

  // TODO: Consider all gRPC return error codes as described in concord_client.proto
  while (!context->IsCancelled()) {
    std::this_thread::sleep_for(10ms);
  }

  client_->unsubscribe();
  return grpc::Status::OK;
}

}  // namespace concord::client::clientservice
