#include "route_guide/route_guide.eventuals.h"

#include "eventuals/concurrent.h"
#include "eventuals/do-all.h"
#include "eventuals/grpc/server.h"
#include "eventuals/just.h"
#include "eventuals/let.h"
#include "eventuals/loop.h"
#include "eventuals/map.h"
#include "eventuals/task.h"
#include "eventuals/then.h"

using eventuals::Concurrent;
using eventuals::DoAll;
using eventuals::Just;
using eventuals::Let;
using eventuals::Loop;
using eventuals::Map;
using eventuals::Task;
using eventuals::Then;

using eventuals::grpc::Stream;

namespace routeguide {
namespace eventuals {

Task<void> RouteGuide::TypeErasedService::Serve() {
  return [this]() {
    return DoAll(
               // GetFeature
               server().Accept<
                   routeguide::RouteGuide,
                   Point,
                   Feature>("GetFeature")
                   | Concurrent([this]() {
                       return Map(Let([this](auto& call) {
                         return UnaryPrologue(call)
                             | Then(Let([&](auto& request) {
                                  return Then(
                                      [&,
                                       // NOTE: using a tuple because need
                                       // to pass more than one
                                       // argument. Also 'this' will be
                                       // downcasted appropriately in
                                       // 'TypeErasedGetFeature()'.
                                       args = std::tuple{
                                           this,
                                           call.context(),
                                           &request}]() mutable {
                                        return TypeErasedGetFeature(&args)
                                            | UnaryEpilogue(call);
                                      });
                                }));
                       }));
                     })
                   | Loop(),

               // ListFeatures
               server().Accept<
                   routeguide::RouteGuide,
                   Rectangle,
                   Stream<Feature>>(
                   "ListFeatures")
                   | Concurrent([this]() {
                       return Map(Let([this](auto& call) {
                         return UnaryPrologue(call)
                             | Then(Let([&](auto& request) {
                                  return Then(
                                      [&,
                                       // NOTE: using a tuple because need
                                       // to pass more than one
                                       // argument. Also 'this' will be
                                       // downcasted appropriately in
                                       // 'TypeErasedListFeatures()'.
                                       args = std::tuple{
                                           this,
                                           call.context(),
                                           &request}]() mutable {
                                        return TypeErasedListFeatures(&args)
                                            | StreamingEpilogue(call);
                                      });
                                }));
                       }));
                     })
                   | Loop(),

               // RecordRoute
               server().Accept<
                   routeguide::RouteGuide,
                   Stream<Point>,
                   RouteSummary>("RecordRoute")
                   | Concurrent([this]() {
                       return Map(Let([this](auto& call) {
                         return Then(
                             [&,
                              // NOTE: using a tuple because need
                              // to pass more than one
                              // argument. Also 'this' will be
                              // downcasted appropriately in
                              // 'TypeErasedRecordRoute()'.
                              args = std::tuple{
                                  this,
                                  call.context(),
                                  &call.Reader()}]() mutable {
                               return TypeErasedRecordRoute(&args)
                                   | UnaryEpilogue(call);
                             });
                       }));
                     })
                   | Loop(),

               // RouteChat
               server().Accept<
                   routeguide::RouteGuide,
                   Stream<RouteNote>,
                   Stream<RouteNote>>(
                   "RouteChat")
                   | Concurrent([this]() {
                       return Map(Let([this](auto& call) {
                         return Then(
                             [&,
                              // NOTE: using a tuple because need
                              // to pass more than one
                              // argument. Also 'this' will be
                              // downcasted appropriately in
                              // 'TypeErasedRouteChat()'.
                              args = std::tuple{
                                  this,
                                  call.context(),
                                  &call.Reader()}]() mutable {
                               return TypeErasedRouteChat(&args)
                                   | StreamingEpilogue(call);
                             });
                       }));
                     })
                   | Loop())
        | Just(); // Return 'void'.
  };
}

} // namespace eventuals
} // namespace routeguide
