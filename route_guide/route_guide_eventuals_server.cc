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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "eventuals/closure.h"
#include "eventuals/filter.h"
#include "eventuals/flat-map.h"
#include "eventuals/grpc/server.h"
#include "eventuals/iterate.h"
#include "eventuals/let.h"
#include "eventuals/lock.h"
#include "eventuals/loop.h"
#include "eventuals/map.h"
#include "eventuals/then.h"
#include "helper.h"
#include "protos/route_guide.eventuals.h"
#include "protos/route_guide.grpc.pb.h"

using routeguide::Point;
using routeguide::Feature;
using routeguide::Rectangle;
using routeguide::RouteSummary;
using routeguide::RouteNote;

using routeguide::eventuals::RouteGuide;

using std::chrono::system_clock;

using eventuals::Closure;
using eventuals::Filter;
using eventuals::FlatMap;
using eventuals::Iterate;
using eventuals::Let;
using eventuals::Loop;
using eventuals::Map;
using eventuals::Synchronizable;
using eventuals::Then;

using eventuals::grpc::Server;
using eventuals::grpc::ServerBuilder;
using eventuals::grpc::ServerReader;

float ConvertToRadians(float num) {
  return num * 3.1415926 /180;
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
  float delta_lat_rad = ConvertToRadians(lat_2-lat_1);
  float delta_lon_rad = ConvertToRadians(lon_2-lon_1);

  float a = pow(sin(delta_lat_rad/2), 2) + cos(lat_rad_1) * cos(lat_rad_2) *
            pow(sin(delta_lon_rad/2), 2);
  float c = 2 * atan2(sqrt(a), sqrt(1-a));
  int R = 6371000; // metres

  return R * c;
}

std::string GetFeatureName(const Point& point,
                           const std::vector<Feature>& feature_list) {
  for (const Feature& f : feature_list) {
    if (f.location().latitude() == point.latitude() &&
        f.location().longitude() == point.longitude()) {
      return f.name();
    }
  }
  return "";
}

class RouteGuideImpl final
  : public RouteGuide::Service<RouteGuideImpl>,
    public Synchronizable {
 public:
  explicit RouteGuideImpl(const std::string& db) {
    routeguide::ParseDb(db, &feature_list_);
  }

  auto GetFeature(grpc::ServerContext* context, Point&& point) {
    Feature feature;
    feature.set_name(GetFeatureName(point, feature_list_));
    feature.mutable_location()->CopyFrom(point);
    return feature;
  }

  auto ListFeatures(
      grpc::ServerContext* context,
      routeguide::Rectangle&& rectangle) {
    auto lo = rectangle.lo();
    auto hi = rectangle.hi();
    long left = (std::min)(lo.longitude(), hi.longitude());
    long right = (std::max)(lo.longitude(), hi.longitude());
    long top = (std::max)(lo.latitude(), hi.latitude());
    long bottom = (std::min)(lo.latitude(), hi.latitude());

    return Iterate(feature_list_)
        | Filter([left, right, top, bottom](const Feature& f) {
             return f.location().longitude() >= left
                 && f.location().longitude() <= right
                 && f.location().latitude() >= bottom
                 && f.location().latitude() <= top;
           });
  }

  auto RecordRoute(grpc::ServerContext* context, ServerReader<Point>& reader) {
    return Closure([this,
                    &reader,
                    point_count = 0,
                    feature_count = 0,
                    distance = 0.0,
                    previous = Point(),
                    start_time = system_clock::now()]() mutable {
      return reader.Read()
          | Map([&](Point&& point) {
               point_count++;
               if (!GetFeatureName(point, feature_list_).empty()) {
                 feature_count++;
               }
               if (point_count != 1) {
                 distance += GetDistance(previous, point);
               }
               previous = point;
             })
          | Loop()
          | Then([&]() {
               system_clock::time_point end_time = system_clock::now();
               RouteSummary summary;
               summary.set_point_count(point_count);
               summary.set_feature_count(feature_count);
               summary.set_distance(static_cast<long>(distance));
               auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                   end_time - start_time);
               summary.set_elapsed_time(secs.count());
               return summary;
             });
    });
  }

  //  route-chat -> (notes :: ...RouteNote) -> ...RouteNote =
  //   ... foreach: notes {
  //     note ->
  //       server is:
  //         Idle { received-notes } ->
  //           set: server is: Chatting
  //           ... foreach: received-notes {
  //             n ->
  //               if: (n `location `latitude) == (note `location `latitude)
  //                   && (n `location `longitude) == (note `location `longitude)
  //               then: ...n
  //               else: ...]
  //             ...] ->
  //               set: received-notes =: `enqueue note
  //               ...]
  //           }
  //         _ -> skip: Idle
  //     ...] -> ...]
  //   }

  auto RouteChat(grpc::ServerContext* context, ServerReader<RouteNote>& reader) {
    return reader.Read()
        | FlatMap(Let(
            [this, notes = std::vector<RouteNote>()](RouteNote& note) mutable {
              return Synchronized(Then([&]() {
                       for (const RouteNote& n : received_notes_) {
                         if (n.location().latitude()
                                 == note.location().latitude()
                             && n.location().longitude()
                                 == note.location().longitude()) {
                           notes.push_back(n);
                         }
                       }
                       received_notes_.push_back(note);
                     }))
                  | Closure([&]() {
                       return Iterate(std::move(notes));
                     });
            }));
  }

 private:
  std::vector<Feature> feature_list_;
  std::vector<RouteNote> received_notes_;
};

int RunServer(const std::string& db_path) {
  std::string server_address("0.0.0.0:50051");
  RouteGuideImpl impl(db_path);

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  builder.RegisterService(&impl);

  auto build = builder.BuildAndStart();

  if (!build.status.ok()) {
    std::cerr << "Failed to build and start server: "
              << build.status.error() << std::endl;
    return -1;
  }

  std::unique_ptr<Server> server(std::move(build.server));
  std::cout << "Server listening on " << server_address << std::endl;

  server->Wait();

  return 0;
}

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);
  return RunServer(db);
}
