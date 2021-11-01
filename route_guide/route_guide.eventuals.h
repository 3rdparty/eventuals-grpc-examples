#include <tuple>

#include "eventuals/generator.h"
#include "eventuals/grpc/server.h"
#include "eventuals/task.h"
#include "eventuals/then.h"
#include "protos/route_guide.grpc.pb.h"

namespace routeguide {
namespace eventuals {

class RouteGuide {
 public:
  static constexpr char const* service_full_name() {
    return routeguide::RouteGuide::service_full_name();
  }

  class TypeErasedService : public ::eventuals::grpc::Service {
   public:
    ::eventuals::Task<void> Serve() override;

    char const* name() override {
      return RouteGuide::service_full_name();
    }

   protected:
    virtual ~TypeErasedService() = default;

    virtual ::eventuals::Task<const Feature&> TypeErasedGetFeature(
        std::tuple<
            TypeErasedService*, // this
            ::grpc::GenericServerContext*,
            Point*>* args) = 0;

    virtual ::eventuals::Generator<const Feature&> TypeErasedListFeatures(
        std::tuple<
            TypeErasedService*, // this
            ::grpc::GenericServerContext*,
            Rectangle*>* args) = 0;

    virtual ::eventuals::Task<const RouteSummary&> TypeErasedRecordRoute(
        std::tuple<
            TypeErasedService*,
            ::grpc::GenericServerContext*,
            ::eventuals::grpc::ServerReader<Point>*>* args) = 0;

    virtual ::eventuals::Generator<const RouteNote&> TypeErasedRouteChat(
        std::tuple<
            TypeErasedService*,
            ::grpc::GenericServerContext*,
            ::eventuals::grpc::ServerReader<RouteNote>*>* args) = 0;
  };

  template <typename Implementation>
  class Service : public TypeErasedService {
    ::eventuals::Task<const Feature&> TypeErasedGetFeature(
        std::tuple<
            TypeErasedService*,
            ::grpc::GenericServerContext*,
            Point*>* args) override {
      return [args]() {
        // NOTE: we have an extra 'Then' here because unary functions
        // don't need to return an eventual and rather than checking
        // if it 'HasValueFrom' we just use 'Then' which handles it.
        return ::eventuals::Then([args]() mutable {
          return std::apply(
              [](auto* implementation, auto* context, auto* request) {
                static_assert(std::is_base_of_v<Service, Implementation>);
                return dynamic_cast<Implementation*>(implementation)
                    ->GetFeature(context, std::move(*request));
              },
              *args);
        });
      };
    }

    ::eventuals::Generator<const Feature&> TypeErasedListFeatures(
        std::tuple<
            TypeErasedService*,
            ::grpc::GenericServerContext*,
            Rectangle*>* args) override {
      return [args]() {
        // NOTE: server streaming functions MUST return a stream so we
        // can just pass on what is returned from 'Implementation'.
        return std::apply(
            [](auto* implementation, auto* context, auto* request) {
              static_assert(std::is_base_of_v<Service, Implementation>);
              return dynamic_cast<Implementation*>(implementation)
                  ->ListFeatures(context, std::move(*request));
            },
            *args);
      };
    }

    ::eventuals::Task<const RouteSummary&> TypeErasedRecordRoute(
        std::tuple<
            TypeErasedService*,
            ::grpc::GenericServerContext*,
            ::eventuals::grpc::ServerReader<Point>*>* args) override {
      return [args]() {
        // NOTE: we have an extra 'Then' here because unary functions
        // don't need to return an eventual and rather than checking
        // if it 'HasValueFrom' we just use 'Then' which handles it.
        return ::eventuals::Then([args]() mutable {
          return std::apply(
              [](auto* implementation, auto* context, auto* reader) {
                static_assert(std::is_base_of_v<Service, Implementation>);
                return dynamic_cast<Implementation*>(implementation)
                    ->RecordRoute(context, *reader);
              },
              *args);
        });
      };
    }

    ::eventuals::Generator<const RouteNote&> TypeErasedRouteChat(
        std::tuple<
            TypeErasedService*,
            ::grpc::GenericServerContext*,
            ::eventuals::grpc::ServerReader<RouteNote>*>* args) override {
      return [args]() {
        // NOTE: server streaming functions MUST return a stream so we
        // can just pass on what is returned from 'Implementation'.
        return std::apply(
            [](auto* implementation, auto* context, auto* reader) {
              static_assert(std::is_base_of_v<Service, Implementation>);
              return dynamic_cast<Implementation*>(implementation)
                  ->RouteChat(context, *reader);
            },
            *args);
      };
    }
  };
};

} // namespace eventuals
} // namespace routeguide
