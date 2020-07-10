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

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "stout/notification.h"

#include "stout/grpc/client.h"

#include "helper.h"

#include "protos/route_guide.grpc.pb.h"

using grpc::Status;

using routeguide::Point;
using routeguide::Feature;
using routeguide::Rectangle;
using routeguide::RouteSummary;
using routeguide::RouteNote;
using routeguide::RouteGuide;

using stout::Notification;

using stout::grpc::Client;
using stout::grpc::ClientCallStatus;
using stout::grpc::Stream;

Point MakePoint(long latitude, long longitude) {
  Point p;
  p.set_latitude(latitude);
  p.set_longitude(longitude);
  return p;
}

Feature MakeFeature(const std::string& name,
                    long latitude, long longitude) {
  Feature f;
  f.set_name(name);
  f.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return f;
}

RouteNote MakeRouteNote(const std::string& message,
                        long latitude, long longitude) {
  RouteNote n;
  n.set_message(message);
  n.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return n;
}

class RouteGuideClient {
 public:
  RouteGuideClient(const std::string& db, Client* client) : client_(client) {
    routeguide::ParseDb(db, &feature_list_);
  }

  void GetFeature() {
    Point point;
    Feature feature;
    point = MakePoint(409146138, -746188906);
    GetOneFeature(point, &feature);
    point = MakePoint(0, 0);
    GetOneFeature(point, &feature);
  }

  void ListFeatures() {
    Rectangle rect;
    Feature feature;

    rect.mutable_lo()->set_latitude(400000000);
    rect.mutable_lo()->set_longitude(-750000000);
    rect.mutable_hi()->set_latitude(420000000);
    rect.mutable_hi()->set_longitude(-730000000);
    std::cout << "Looking for features between 40, -75 and 42, -73"
              << std::endl;

    Notification<Status> done;
    client_->Call<RouteGuide, Rectangle, Stream<Feature>>(
        "ListFeatures",
        &rect,
        [this](auto* call, auto&& feature) {
          if (feature) {
            std::cout << "Found feature called "
                      << feature->name() << " at "
                      << feature->location().latitude()/kCoordFactor_ << ", "
                      << feature->location().longitude()/kCoordFactor_ << std::endl;
          } else {
            call->Finish();
          }
        },
        [&](auto*, const Status& status) {
          done.Notify(status);
        });

    Status status = done.Wait();
    if (status.ok()) {
      std::cout << "ListFeatures rpc succeeded." << std::endl;
    } else {
      std::cout << "ListFeatures rpc failed." << std::endl;
    }
  }

  void RecordRoute() {
    Point point;
    RouteSummary stats;

    const int kPoints = 10;
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();

    std::default_random_engine generator(seed);
    std::uniform_int_distribution<int> feature_distribution(
        0, feature_list_.size() - 1);
    std::uniform_int_distribution<int> delay_distribution(
        500, 1500);

    Notification<Status> done;
    client_->Call<RouteGuide, Stream<Point>, RouteSummary>(
        "RecordRoute",
        [&](auto&& call, bool ok) {
          call->OnFinished([&](auto*, const Status& status) {
            done.Notify(status);
          });
          if (!ok) {
            call->context()->TryCancel();
            call->Finish();
          } else {
            for (int i = 0; i < kPoints; i++) {
              const Feature& f = feature_list_[feature_distribution(generator)];
              std::cout << "Visiting point "
                        << f.location().latitude()/kCoordFactor_ << ", "
                        << f.location().longitude()/kCoordFactor_ << std::endl;
              if (call->Write(f.location()) != ClientCallStatus::Ok) {
                // Broken stream.
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(
                  delay_distribution(generator)));
            }
            call->WritesDone();
            call->OnRead([&](auto* call, auto&& response) {
              stats.Swap(response.get());
              call->Finish();
            });
          }
        });
    Status status = done.Wait();
    if (status.ok()) {
      std::cout << "Finished trip with " << stats.point_count() << " points\n"
                << "Passed " << stats.feature_count() << " features\n"
                << "Travelled " << stats.distance() << " meters\n"
                << "It took " << stats.elapsed_time() << " seconds"
                << std::endl;
    } else {
      std::cout << "RecordRoute rpc failed." << std::endl;
    }
  }

  void RouteChat() {
    std::thread writer;
    Notification<Status> done;
    client_->Call<RouteGuide, Stream<RouteNote>, Stream<RouteNote>>(
        "RouteChat",
        [&](auto&& call, bool ok) {
          call->OnFinished([&](auto*, const Status& status) {
            done.Notify(status);
          });
          if (!ok) {
            call->context()->TryCancel();
            call->Finish();
          } else {
            call->OnRead([](auto*, auto&& server_note) {
              if (server_note) {
                std::cout << "Got message " << server_note->message()
                          << " at " << server_note->location().latitude() << ", "
                          << server_note->location().longitude() << std::endl;
              }
            });
            writer = std::thread([&, call = std::move(call)]() {
              std::vector<RouteNote> notes{
                MakeRouteNote("First message", 0, 0),
                  MakeRouteNote("Second message", 0, 1),
                  MakeRouteNote("Third message", 1, 0),
                  MakeRouteNote("Fourth message", 0, 0)};
              for (const RouteNote& note : notes) {
                std::cout << "Sending message " << note.message()
                          << " at " << note.location().latitude() << ", "
                          << note.location().longitude() << std::endl;
                call->Write(note);
              }
              call->WritesDoneAndFinish();
            });
          }
        });

    writer.join();
    Status status = done.Wait();
    if (!status.ok()) {
      std::cout << "RouteChat rpc failed." << std::endl;
    }
  }

 private:

  bool GetOneFeature(const Point& point, Feature* feature) {
    Notification<Status> done;
    client_->Call<RouteGuide, Point, Feature>(
        "GetFeature",
        &point,
        [&](auto* call, auto&& response) {
          feature->Swap(response.get());
          call->Finish();
        },
        [&](auto*, const Status& status) {
          done.Notify(status);
        });
    Status status = done.Wait();
    if (!status.ok()) {
      std::cout << "GetFeature rpc failed." << std::endl;
      return false;
    }
    if (!feature->has_location()) {
      std::cout << "Server returns incomplete feature." << std::endl;
      return false;
    }
    if (feature->name().empty()) {
      std::cout << "Found no feature at "
                << feature->location().latitude()/kCoordFactor_ << ", "
                << feature->location().longitude()/kCoordFactor_ << std::endl;
    } else {
      std::cout << "Found feature called " << feature->name()  << " at "
                << feature->location().latitude()/kCoordFactor_ << ", "
                << feature->location().longitude()/kCoordFactor_ << std::endl;
    }
    return true;
  }

  const float kCoordFactor_ = 10000000.0;
  Client* client_;
  std::vector<Feature> feature_list_;
};

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);
  Client client("localhost:50051", grpc::InsecureChannelCredentials());
  RouteGuideClient guide(db, &client);

  std::cout << "-------------- GetFeature --------------" << std::endl;
  guide.GetFeature();
  std::cout << "-------------- ListFeatures --------------" << std::endl;
  guide.ListFeatures();
  std::cout << "-------------- RecordRoute --------------" << std::endl;
  guide.RecordRoute();
  std::cout << "-------------- RouteChat --------------" << std::endl;
  guide.RouteChat();

  return 0;
}
