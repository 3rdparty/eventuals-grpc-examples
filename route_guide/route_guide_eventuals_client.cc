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

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "eventuals/do-all.h"
#include "eventuals/event-loop.h"
#include "eventuals/finally.h"
#include "eventuals/foreach.h"
#include "eventuals/grpc/client.h"
#include "eventuals/head.h"
#include "eventuals/iterate.h"
#include "eventuals/let.h"
#include "eventuals/range.h"
#include "eventuals/then.h"
#include "eventuals/timer.h"
#include "helper.h"
#include "protos/route_guide.grpc.pb.h"

using grpc::ClientContext;
using grpc::Status;

using routeguide::Point;
using routeguide::Feature;
using routeguide::Rectangle;
using routeguide::RouteSummary;
using routeguide::RouteNote;
using routeguide::RouteGuide;

using stout::Borrowable;

using eventuals::DoAll;
using eventuals::EventLoop;
using eventuals::Finally;
using eventuals::Foreach;
using eventuals::Head;
using eventuals::Iterate;
using eventuals::Let;
using eventuals::Range;
using eventuals::Then;
using eventuals::Timer;

using eventuals::grpc::Client;
using eventuals::grpc::CompletionPool;
using eventuals::grpc::Stream;

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
  RouteGuideClient(
      const std::string& target,
      const std::shared_ptr<::grpc::ChannelCredentials>& credentials,
      stout::borrowed_ptr<CompletionPool> pool,
      const std::string& db)
    : client_(target, credentials, std::move(pool)) {
    routeguide::ParseDb(db, &feature_list_);
  }

  auto GetFeature();

  auto ListFeatures() {
    return client_.Call<RouteGuide, Rectangle, Stream<Feature>>("ListFeatures")
        | Then(Let([this](auto& call) {
             routeguide::Rectangle rect;

             rect.mutable_lo()->set_latitude(400000000);
             rect.mutable_lo()->set_longitude(-750000000);
             rect.mutable_hi()->set_latitude(420000000);
             rect.mutable_hi()->set_longitude(-730000000);
             std::cout << "Looking for features between 40, -75 and 42, -73"
                       << std::endl;

             return call.Writer().WriteLast(rect)
                 | Foreach(
                        call.Reader().Read(),
                        ([&](Feature&& feature) {
                          auto latitude = feature.location().latitude();
                          auto longitude = feature.location().longitude();

                          std::cout << "Found feature called "
                                    << feature.name() << " at "
                                    << latitude / kCoordFactor_ << ", "
                                    << longitude / kCoordFactor_ << std::endl;
                        }))
                 | Finally([&](auto) {
                      return call.Finish();
                    })
                 | Then([](Status&& status) {
                      if (status.ok()) {
                        std::cout << "ListFeatures rpc succeeded." << std::endl;
                      } else {
                        std::cout << "ListFeatures rpc failed." << std::endl;
                      }
                    });
           }));
  }

  auto RecordRoute() {
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    return client_.Call<RouteGuide, Stream<Point>, RouteSummary>("RecordRoute")
        | Then(Let(
            [this,
             generator = std::default_random_engine(seed),
             feature_distribution = std::uniform_int_distribution<int>(
                 0,
                 feature_list_.size() - 1),
             delay_distribution = std::uniform_int_distribution<int>(
                 500,
                 1500)](auto& call) mutable {
              const int kPoints = 10;
              return Foreach(
                         Range(kPoints),
                         ([&](int) {
                           const Feature& f =
                               feature_list_[feature_distribution(generator)];

                           auto latitude = f.location().latitude();
                           auto longitude = f.location().longitude();

                           std::cout << "Visiting point "
                                     << latitude / kCoordFactor_ << ", "
                                     << longitude / kCoordFactor_ << std::endl;

                           return call.Writer().Write(f.location())
                               | Timer(std::chrono::milliseconds(
                                   delay_distribution(generator)));
                         }))
                  | call.WritesDone()
                  | call.Reader().Read()
                  | Head()
                  | Finally(Let([&](auto& stats) {
                       return call.Finish()
                           | Then([&](Status&& status) {
                                if (status.ok() && stats) {
                                  std::cout
                                      << "Finished trip with "
                                      << stats->point_count() << " points\n"
                                      << "Passed " << stats->feature_count()
                                      << " features\n"
                                      << "Travelled " << stats->distance()
                                      << " meters\n"
                                      << "It took " << stats->elapsed_time()
                                      << " seconds"
                                      << std::endl;
                                } else {
                                  std::cout
                                      << "RecordRoute rpc failed." << std::endl;
                                }
                              });
                     }));
            }));
  }

  auto RouteChat() {
    return client_.Call<
               RouteGuide,
               Stream<RouteNote>,
               Stream<RouteNote>>("RouteChat")
        | Then(Let([](auto& call) {
             return DoAll(
                        Foreach(
                            Iterate(
                                {MakeRouteNote("First message", 0, 0),
                                 MakeRouteNote("Second message", 0, 1),
                                 MakeRouteNote("Third message", 1, 0),
                                 MakeRouteNote("Fourth message", 0, 0)}),
                            [&](RouteNote&& note) {
                              std::cout
                                  << "Sending message " << note.message()
                                  << " at " << note.location().latitude()
                                  << ", " << note.location().longitude()
                                  << std::endl;
                              return call.Writer().Write(note);
                            })
                            | call.WritesDone(),
                        Foreach(
                            call.Reader().Read(),
                            [&](RouteNote&& note) {
                              std::cout
                                  << "Got message " << note.message()
                                  << " at " << note.location().latitude()
                                  << ", " << note.location().longitude()
                                  << std::endl;
                            }))
                 | Finally([&](auto) {
                      return call.Finish();
                    })
                 | Then([](Status&& status) {
                      if (!status.ok()) {
                        std::cout << "RouteChat rpc failed." << std::endl;
                      }
                    });
           }));
  }

 private:
  auto GetOneFeature(Point&& point) {
    return client_.Call<RouteGuide, Point, Feature>("GetFeature")
        | Then(Let([this, point = std::move(point)](auto& call) {
             return call.Writer().WriteLast(point)
                 | call.Reader().Read()
                 | Head()
                 | Finally(Let([&](auto& feature) {
                      return call.Finish()
                          | Then([&](Status&& status) {
                               if (!status.ok() || !feature) {
                                 std::cout
                                     << "GetFeature rpc failed." << std::endl;
                                 return false;
                               }

                               if (!feature->has_location()) {
                                 std::cout
                                     << "Server returns incomplete feature."
                                     << std::endl;
                                 return false;
                               }

                               auto latitude = feature->location().latitude();
                               auto longitude = feature->location().longitude();

                               if (feature->name().empty()) {
                                 std::cout
                                     << "Found no feature at "
                                     << latitude / kCoordFactor_ << ", "
                                     << longitude / kCoordFactor_ << std::endl;
                               } else {
                                 std::cout
                                     << "Found feature called "
                                     << feature->name() << " at "
                                     << latitude / kCoordFactor_ << ", "
                                     << longitude / kCoordFactor_ << std::endl;
                               }
                               return true;
                             });
                    }));
           }));
  }

  const float kCoordFactor_ = 10000000.0;
  Client client_;
  std::vector<Feature> feature_list_;
};

auto RouteGuideClient::GetFeature() {
  return GetOneFeature(MakePoint(409146138, -746188906))
      | Then([this](bool) {
           return GetOneFeature(MakePoint(0, 0));
         });
}

int main(int argc, char** argv) {
  EventLoop::ConstructDefaultAndRunForeverDetached();
    
  Borrowable<CompletionPool> pool;

  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);
  RouteGuideClient guide(
      "localhost:50051",
      grpc::InsecureChannelCredentials(),
      pool.Borrow(),
      db);

  std::cout << "-------------- GetFeature --------------" << std::endl;
  *guide.GetFeature();
  std::cout << "-------------- ListFeatures --------------" << std::endl;
  *guide.ListFeatures();
  std::cout << "-------------- RecordRoute --------------" << std::endl;
  *guide.RecordRoute();
  std::cout << "-------------- RouteChat --------------" << std::endl;
  *guide.RouteChat();

  return 0;
}
