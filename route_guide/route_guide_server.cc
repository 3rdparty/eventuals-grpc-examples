/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <google/protobuf/util/message_differencer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "helper.h"
#include "protos/route_guide.grpc.pb.h"
#include "stout/grpc/server.h"
#include "stout/head.h"
#include "stout/loop.h"
#include "stout/map.h"
#include "stout/terminal.h"
#include "stout/then.h"

using grpc::Status;
using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;
using std::chrono::system_clock;
using stout::Borrowable;
using stout::eventuals::Head;
using stout::eventuals::Loop;
using stout::eventuals::Map;
using stout::eventuals::Terminate;
using stout::eventuals::Then;
using stout::eventuals::grpc::Server;
using stout::eventuals::grpc::ServerBuilder;
using stout::eventuals::grpc::Stream;


float ConvertToRadians(float num) {
  return num * 3.1415926 / 180;
}

// The formula is based on http://mathforum.org/library/drmath/view/51879.html
float GetDistance(const Point& start, const Point& end) {
  const float kCoordFactor = 10000000.0;
  float lat_1 = start.latitude() / kCoordFactor;
  float lat_2 = end.latitude() / kCoordFactor;
  float lon_1 = start.longitude() / kCoordFactor;
  float lon_2 = end.longitude() / kCoordFactor;
  float lat_rad_1 = ConvertToRadians(lat_1);
  float lat_rad_2 = ConvertToRadians(lat_2);
  float delta_lat_rad = ConvertToRadians(lat_2 - lat_1);
  float delta_lon_rad = ConvertToRadians(lon_2 - lon_1);

  float a = pow(sin(delta_lat_rad / 2), 2)
      + cos(lat_rad_1) * cos(lat_rad_2) * pow(sin(delta_lon_rad / 2), 2);
  float c = 2 * atan2(sqrt(a), sqrt(1 - a));
  int R = 6371000; // metres

  return R * c;
}

// Return the name of the feature in the `feature_list` vector that's
// associated with the given `point`.
std::string GetFeatureName(
    const Point& point,
    const std::vector<Feature>& feature_list) {
  for (const Feature& f : feature_list) {
    if (google::protobuf::util::MessageDifferencer::Equals(
            f.location(),
            point)) {
      return f.name();
    }
  }
  return "";
}


// The RouteGuideImpl class demonstrates server-side use of stout-grpc,
// which is a higher-level interface on top of gRPC while still being
// asynchronous. It uses stout-eventuals
// (https://github.com/3rdparty/stout-eventuals) to make composing async code
// easier.
//
// This class contains 4 methods: ServeGetFeature (unary rpc),
// ServeListFeatures (server streaming), ServeRecordRoute (client streaming),
// and ServeRouteChat (bidirectional streaming).
class RouteGuideImpl final {
 public:
  // Populate feature_list_ vector with addresses from database
  // (route_guide_db.json).
  explicit RouteGuideImpl(const std::string& db) {
    routeguide::ParseDb(db, &feature_list_);
  }

  // ServeGetFeature serves unary (one-request, one-response) rpc calls.
  // The client sends the server a request containing a Point (latitude,
  // longitude). If the server knows the address that corresponds to the
  // Point's coordinates it will response with that address.
  //
  // The `Accept<...>()` method (stout/grpc/server.h) gets the name of the
  // RPC function, and creates an "Eventual" that will produce a stream of
  // connections. Eventuals can be composed with other Eventuals using
  // the "|" operator, and here we compose `Accept` with a subsequent
  // `Map` to handle each connection in the stream with a `Server::Handler()`.
  // We end the chain of Eventuals processing each connection by composing
  // with `Loop`, which takes us back to the top of the streaming chain to
  // process the next connection.
  auto ServeGetFeature(Server* server) {
    return server->Accept<
               RouteGuide, // Service name.
               Point, // Request from client.
               Feature>( // Response from the server.
               "GetFeature")
        | Map(Then([this](auto&& context) {
             // The Server::Handler() can be composed with other tasks using
             // the "|" operator.
             // Its .body() callback is invoked every time the client sends a
             // request to the server.
             return Server::Handler(std::move(context))
                 .body([this](auto& call, auto&& point) {
                   // Check if the request is valid.
                   if (point.get() == nullptr) {
                     return;
                   }
                   Feature feature;
                   // Populate the response message with data.
                   feature.set_name(
                       GetFeatureName(*point.get(), feature_list_));
                   feature.mutable_location()->CopyFrom(*point.get());
                   // Send the response to the client and finish the call.
                   call.WriteAndFinish(feature, Status::OK);
                 });
           }))
        | Loop();
  }

  // ServeListFeatures() function demonstrates a server streaming RPC. The
  // client sends Rectangle messages, the server responds with a stream
  // of addresses that fall within the Rectangle's area.
  auto ServeListFeatures(Server* server) {
    return server->Accept<
               RouteGuide,
               Rectangle,
               Stream<Feature>>("ListFeatures")
        | Map(Then([this](auto&& context) {
             // The Server::Handler() can be composed with other tasks using
             // the "|" operator.
             // Its .body() callback is invoked every time the client sends a
             // request to the server.
             return Server::Handler(std::move(context))
                 .body([this](auto& call, auto&& rectangle) {
                   // Check if the request is valid.
                   if (rectangle.get() == nullptr) {
                     return;
                   }
                   auto lo = rectangle->lo();
                   auto hi = rectangle->hi();
                   long left = std::min(lo.longitude(), hi.longitude());
                   long right = std::max(lo.longitude(), hi.longitude());
                   long top = std::max(lo.latitude(), hi.latitude());
                   long bottom = std::min(lo.latitude(), hi.latitude());
                   for (const Feature& f : feature_list_) {
                     if (f.location().longitude() >= left
                         && f.location().longitude() <= right
                         && f.location().latitude() >= bottom
                         && f.location().latitude() <= top) {
                       // The response to the client.
                       call.Write(f);
                     }
                   }
                   // When writes are done just finish the call.
                   call.Finish(Status::OK);
                 });
           }))
        | Loop();
  }

  // ServeRecordRoute() demonstrates a client streaming RPC. The  client
  // sends some number of Point messages to the server. The server
  // responds with a single message which contains the number of points,
  // number of features found in the db, the resulting distance calculated from
  // all Point coordinates, and the resulting time.
  auto ServeRecordRoute(Server* server) {
    return server->Accept<
               RouteGuide,
               Stream<Point>,
               RouteSummary>("RecordRoute")
        | Map(Then([this](auto&& context) {
             int point_count = 0;
             int feature_count = 0;
             float distance = 0.f;
             Point previous;
             system_clock::time_point start_time = system_clock::now();
             return Server::Handler(std::move(context))
                 .body([this,
                        point_count,
                        feature_count,
                        distance,
                        previous,
                        start_time](
                           auto& call,
                           auto&& point) mutable {
                   // Check if the request is valid.
                   if (point.get() != nullptr) {
                     point_count++;
                     if (!GetFeatureName(*point, feature_list_).empty()) {
                       feature_count++;
                     }
                     if (point_count != 1) {
                       distance += GetDistance(previous, *point);
                     }
                     previous = *point;
                   } else {
                     // The invalid request indicates the client has finished
                     // sending all its requests. We can send the final
                     // response.
                     system_clock::time_point end_time = system_clock::now();
                     RouteSummary summary;
                     summary.set_point_count(point_count);
                     summary.set_feature_count(feature_count);
                     summary.set_distance(static_cast<long>(distance));
                     auto secs = std::chrono::duration_cast<
                         std::chrono::seconds>(
                         end_time - start_time);
                     summary.set_elapsed_time(secs.count());
                     call.WriteAndFinish(summary, Status::OK);
                   }
                 });
           }))
        | Loop();
  }

  // ServeRouteChat demonstrates a bidirectional streaming RPC. The client
  // sends a sequence of RouteNote messages (see protos/route_guide.proto),
  // which each have a location. The server replies with any RouteNotes that it
  // has previously received for those locations.
  auto ServeRouteChat(Server* server) {
    return server->Accept<
               RouteGuide,
               Stream<RouteNote>,
               Stream<RouteNote>>("RouteChat")
        | Map(Then([this](auto&& context) {
             return Server::Handler(std::move(context))
                 .body([this](auto& call, auto&& note) {
                   // Check if the request is valid.
                   if (note.get() == nullptr) {
                     // When writes are done just finish call.
                     call.Finish(Status::OK);
                     return;
                   }
                   for (const RouteNote& n : received_notes_) {
                     if (n.location().latitude()
                             == note->location().latitude()
                         && n.location().longitude()
                             == note->location().longitude()) {
                       // Response to the client.
                       call.Write(n);
                     }
                   }
                   received_notes_.push_back(*note);
                 });
           }))
        | Loop();
  }

 private:
  std::vector<Feature> feature_list_;
  std::vector<RouteNote> received_notes_;
};

int RunServer(const std::string& db_path) {
  std::string server_address("0.0.0.0:50051");
  RouteGuideImpl service(db_path);

  // Build a server using a ServerBuilder just like with gRPC.
  ServerBuilder builder;
  builder.AddListeningPort(
      server_address,
      grpc::InsecureServerCredentials());

  // Unlike with gRPC, BuildAndStart() performs validation and returns a
  // "named-tuple" with a status and server field to better handle errors.
  auto build = builder.BuildAndStart();

  if (!build.status.ok()) {
    std::cerr << "Failed to build and start server: "
              << build.status.error() << std::endl;
    return -1;
  }
  // Create the server.
  std::unique_ptr<Server> server(std::move(build.server));
  std::cout << "Server listening on " << server_address << std::endl;

  // Serve all of the RPC methods. Since Loop() returns an eventual, we
  // need terminate our eventual and then start it explicitly with Start().
  auto [future1, k1] = Terminate(service.ServeGetFeature(server.get()));
  auto [future2, k2] = Terminate(service.ServeListFeatures(server.get()));
  auto [future3, k3] = Terminate(service.ServeRecordRoute(server.get()));
  auto [future4, k4] = Terminate(service.ServeRouteChat(server.get()));
  k1.Start();
  k2.Start();
  k3.Start();
  k4.Start();
  server->Wait();

  return 0;
}

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);
  return RunServer(db);
}
