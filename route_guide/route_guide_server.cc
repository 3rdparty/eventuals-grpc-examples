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

#include "stout/grpc/server.h"

#include "helper.h"

#include "protos/route_guide.grpc.pb.h"

using grpc::Status;
using routeguide::Point;
using routeguide::Feature;
using routeguide::Rectangle;
using routeguide::RouteSummary;
using routeguide::RouteNote;
using routeguide::RouteGuide;
using std::chrono::system_clock;
using stout::grpc::Server;
using stout::grpc::ServerBuilder;
using stout::grpc::Stream;


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

class RouteGuideImpl final {
 public:
  explicit RouteGuideImpl(const std::string& db) {
    routeguide::ParseDb(db, &feature_list_);
  }

  auto Serve(Server* server) {
    auto status = server->Serve<RouteGuide, Point, Feature>(
        "GetFeature",
        [this](auto* call, auto&& point) {
          Feature feature;
          feature.set_name(GetFeatureName(*point, feature_list_));
          feature.mutable_location()->CopyFrom(*point);
          call->WriteAndFinish(feature, Status::OK);
        },
        [](auto*, bool) {});

    if (!status.ok()) {
      return status;
    }

    status = server->Serve<RouteGuide, Rectangle, Stream<Feature>>(
        "ListFeatures",
        [this](auto* call, auto&& rectangle) {
          auto lo = rectangle->lo();
          auto hi = rectangle->hi();
          long left = (std::min)(lo.longitude(), hi.longitude());
          long right = (std::max)(lo.longitude(), hi.longitude());
          long top = (std::max)(lo.latitude(), hi.latitude());
          long bottom = (std::min)(lo.latitude(), hi.latitude());
          for (const Feature& f : feature_list_) {
            if (f.location().longitude() >= left &&
                f.location().longitude() <= right &&
                f.location().latitude() >= bottom &&
                f.location().latitude() <= top) {
              call->Write(f);
            }
          }
          call->Finish(Status::OK);
        },
        [](auto*, bool) {});

    if (!status.ok()) {
      return status;
    }

    status = server->Serve<RouteGuide, Stream<Point>, RouteSummary>(
        "RecordRoute",
        [this](auto&& call) {
          int point_count = 0;
          int feature_count = 0;
          float distance = 0.0;
          Point previous;

          system_clock::time_point start_time = system_clock::now();

          call->OnRead(
              [this, point_count, feature_count, distance, previous, start_time](
                  stout::grpc::ServerCall<Stream<Point>, RouteSummary>* call, auto&& point) mutable {
                  // auto* call, auto&& point) mutable {
            if (point) {
              point_count++;
              if (!GetFeatureName(*point, feature_list_).empty()) {
                feature_count++;
              }
              if (point_count != 1) {
                distance += GetDistance(previous, *point);
              }
              previous = *point;
            } else {
              system_clock::time_point end_time = system_clock::now();
              RouteSummary summary;
              summary.set_point_count(point_count);
              summary.set_feature_count(feature_count);
              summary.set_distance(static_cast<long>(distance));
              auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                  end_time - start_time);
              summary.set_elapsed_time(secs.count());
              call->WriteAndFinish(summary, Status::OK);
            }
          });
        });

    if (!status.ok()) {
      return status;
    }

    status = server->Serve<RouteGuide, Stream<RouteNote>, Stream<RouteNote>>(
        "RouteChat",
        [this](auto* call, auto&& note) {
          if (note) {
            std::unique_lock<std::mutex> lock(mu_);
            for (const RouteNote& n : received_notes_) {
              if (n.location().latitude() == note->location().latitude() &&
                  n.location().longitude() == note->location().longitude()) {
                call->Write(n);
              }
            }
            received_notes_.push_back(*note);
          } else {
            call->Finish(Status::OK);
          }
        },
        [](auto*, bool) {});

    return status;
  }

 private:
  std::vector<Feature> feature_list_;
  std::mutex mu_;
  std::vector<RouteNote> received_notes_;
};

int RunServer(const std::string& db_path) {
  std::string server_address("0.0.0.0:50051");
  RouteGuideImpl service(db_path);

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  auto build = builder.BuildAndStart();

  if (!build.status.ok()) {
    std::cerr << "Failed to build and start server: "
              << build.status.error() << std::endl;
    return -1;
  }

  std::unique_ptr<Server> server(std::move(build.server));
  std::cout << "Server listening on " << server_address << std::endl;

  auto status = service.Serve(server.get());

  if (!status.ok()) {
    std::cerr << "Failed to serve: " << status.error() << std::endl;
    return -1;
  }

  server->Wait();

  return 0;
}

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);
  return RunServer(db);
}
