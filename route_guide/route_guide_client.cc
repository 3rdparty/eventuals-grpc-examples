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

#include "helper.h"
#include "protos/route_guide.grpc.pb.h"
#include "stout/borrowed_ptr.h"
#include "stout/grpc/client.h"
#include "stout/notification.h"
#include "stout/sequence.h"
#include "stout/terminal.h"

using grpc::Status;

using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;

using stout::Borrowable;
using stout::Notification;
using stout::Sequence;

using stout::eventuals::Terminal;
using stout::eventuals::Terminate;
using stout::eventuals::grpc::Client;
using stout::eventuals::grpc::CompletionPool;
using stout::eventuals::grpc::Stream;

// Populate Point message (you can check this message structure in
// route_guide.proto)
Point MakePoint(long latitude, long longitude) {
  Point p;
  p.set_latitude(latitude);
  p.set_longitude(longitude);
  return p;
}

// Populate Feature message
Feature MakeFeature(const std::string& name, long latitude, long longitude) {
  Feature f;
  f.set_name(name);
  f.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return f;
}

// Populate RouteNote message
RouteNote MakeRouteNote(const std::string& message, long latitude,
                        long longitude) {
  RouteNote n;
  n.set_message(message);
  n.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return n;
}

// The basic idea of stout-grpc is based on the callbacks. stout-grpc is
// intented to be a higher-level interface while still being asynchronous
// that uses stout-eventuals (https://github.com/3rdparty/stout-eventuals)
// to make composing async code easier.

class RouteGuideClient {
 public:
  // Populate feature_list_ vector with the data from db(route_guide_db.json)
  RouteGuideClient(const std::string& db, Client* client) : client_(client) {
    routeguide::ParseDb(db, &feature_list_);
  }

 public:
  // The goal of GetFeature is just to implement async unary rpc call.
  // Client sends to the server the request which contains Point's coordi-
  // nates (longitude, latitude). The server checks received Point's coordi-
  // nates, if database has the specific address which corresponds to Point's
  // coordinates - the server will response with it address. GetFeature
  // returns lambda.
  auto GetFeature(long latitude, long longitude) {
    // The idea of Data structure is just to use it in the .context() callback
    // in order to have the way of injecting some data on a stack, or as
    // a part of the eventual data structure and manipulate with this data in
    // multiple callbacks (.start(), .fail(), .ready(), .body(), etc).
    struct Data {
      Point point;      // Request from the client
      Feature feature;  // Response from the server
    };

    return [latitude, longitude, this]() {
      // Call() function gets the rpc name and the host name as parameters
      // and returns Composable structure which is composed with Handler()
      // call using overloaded operator "|" in order to handle calls from
      // server. Actually Handler() returns Composable structure too, so it
      // can be composed with other tasks. (You can check Composable
      // structure's implementation in the stout/grpc directory in handler.h)
      return client_->Call<RouteGuide, Point, Feature>("GetFeature") |
             (Client::Handler()
                  // Passing Data structure to the context
                  .context(Data{MakePoint(latitude, longitude), Feature{}})
                  // Send the request with Point's data to the server. First
                  // argument in the .ready() callback gets the context. The
                  // second argument gets the call. You can send requests to
                  // the server using Write*() family functions. In this
                  // case we just send a unique request and finish our call
                  // with WriteLast function.
                  .ready([](auto&& data, auto& call) {
                    call.WriteLast(data.point);
                  })
                  // .body() callback will be invoked every time the server
                  // will send the response. First argument in lambda gets
                  // the context, second - the call, third - the response
                  // object from the server.
                  .body([this](auto&& data, auto& call, auto&& response) {
                    // Do not forget to check if the response from the server
                    // is valid! If so, consequently we process the response
                    // from the server.
                    if (response) {
                      data.feature.Swap(response.get());
                      if (!data.feature.has_location()) {
                        std::cout << "Server returns incomplete feature."
                                  << std::endl;
                      }
                      if (data.feature.name().empty()) {
                        std::cout << "Found no feature at "
                                  << data.feature.location().latitude() /
                                         kCoordFactor_
                                  << ", "
                                  << data.feature.location().longitude() /
                                         kCoordFactor_
                                  << std::endl;
                      } else {
                        std::cout << "Found feature called "
                                  << data.feature.name() << " at "
                                  << data.feature.location().latitude() /
                                         kCoordFactor_
                                  << ", "
                                  << data.feature.location().longitude() /
                                         kCoordFactor_
                                  << std::endl;
                      }
                    }
                  })
                  // .finished callback is invoked when the call is finished.
                  // In this callback we just start our continuation.
                  .finished([](auto&& data, auto& call, auto&& resp) {
                    call.Start(grpc::Status::OK);
                  }));
    };
  }

  // ListFeatures() function implements the server streaming RPC. The client
  // sends Rectangle's message to the server, the server responds with a
  // sequence of messages(stream) which contain specific address which hits
  // the Rectangle area. The pattern is quite the same as in GetFeauture().
  auto ListFeatures() {
    struct Data {
      Rectangle rectangle;  // Request from the client
      Feature feature;      // Response from the server
    };
    return [this]() {
      return client_->Call<RouteGuide, Rectangle, Stream<Feature>>(
                 "ListFeatures") |
             Client::Handler()
                 .context(Data{Rectangle{}, Feature{}})
                 // Prepare the rectangle area and send to the server as a
                 // request.
                 .ready([](auto&& data, auto& call) {
                   data.rectangle.mutable_lo()->set_latitude(400000000);
                   data.rectangle.mutable_lo()->set_longitude(-750000000);
                   data.rectangle.mutable_hi()->set_latitude(420000000);
                   data.rectangle.mutable_hi()->set_longitude(-730000000);
                   std::cout << "Looking for features between 40, -75"
                             << " and 42, -73" << std::endl;
                   call.WriteLast(data.rectangle);
                 })
                 // .body() callback will be invoked every time the server
                 // will send the response. The response contains the specific
                 // address which hits the requested rectangle area.
                 .body([this](auto&& data, auto& call, auto&& feature) {
                   // Check if the response froom the server is valid
                   if (feature) {
                     std::cout
                         << "Found feature called " << feature->name()
                         << " at "
                         << feature->location().latitude() / kCoordFactor_
                         << ", "
                         << feature->location().longitude() / kCoordFactor_
                         << std::endl;
                   }
                 })
                 // .finished callback is invoked when the call is finished.
                 // In this callback we just start our continuation.
                 .finished([](auto&& data, auto& call, auto&& response) {
                   call.Start(grpc::Status::OK);
                 });
    };
  }

  // RecordRoute() function implements the client streaming RPC. The client
  // sends 10 Point's messages to the server. Each message consists of the
  // Point's data stucture. The server responds with a unique message which
  // contains the number of points, number of feautures found in the db,
  // the resulting distance calculated by using all Point's coordinates and
  // the resulting time.
  auto RecordRoute() {
    struct Data {
      size_t kPoints;      // The number of points to send to the server
      Point point;         // The request to the server
      RouteSummary stats;  // The response from the server
    };

    return [this]() {
      return client_->Call<RouteGuide, Stream<Point>, RouteSummary>(
                 "RecordRoute") |
             Client::Handler()
                 .context(Data{10, Point{}, RouteSummary{}})
                 // Preparing 10 points for the request. So, the idea of this
                 // callback is to choose randomly 10 points from the db. After
                 // sending every request there is also a little delay in order
                 // to simulate "long" travelling from one point to another.
                 .ready([this](auto&& data, auto& call) {
                   unsigned seed = std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count();

                   std::default_random_engine generator(seed);
                   std::uniform_int_distribution<int> feature_distribution(
                       0, feature_list_.size() - 1);
                   std::uniform_int_distribution<int> delay_distribution(500,
                                                                         1500);

                   for (int i = 0; i < data.kPoints; i++) {
                     const Feature& f =
                         feature_list_[feature_distribution(generator)];
                     std::cout << "Visiting point "
                               << f.location().latitude() / kCoordFactor_
                               << ", "
                               << f.location().longitude() / kCoordFactor_
                               << std::endl;
                     call.Write(f.location());
                     std::this_thread::sleep_for(std::chrono::milliseconds(
                         delay_distribution(generator)));
                   }
                   // When writes are done we should call WritesDone in order
                   // to finish our call.
                   call.WritesDone();
                 })

                 // .body() callback will be invoked every time the server
                 // will send the response.
                 .body([](auto&& data, auto& call, auto&& response) {
                   // We should check if the response is valid from the server
                   if (response) {
                     data.stats.Swap(response.get());
                     std::cout << "Finished trip with "
                               << data.stats.point_count() << " points\n"
                               << "Passed " << data.stats.feature_count()
                               << " features\n"
                               << "Travelled " << data.stats.distance()
                               << " meters\n"
                               << "It took " << data.stats.elapsed_time()
                               << " seconds" << std::endl;
                   }
                 })
                 // .finished callback is invoked when the call is finished.
                 // In this callback we just start our continuation.
                 .finished([](auto&& data, auto& call, auto&& response) {
                   call.Start(grpc::Status::OK);
                 });
    };
  }

  // The main goal of the RouteChat() function is to implement bidirectional
  // streaming RPC. The client sends the sequence of 4 messages. Each message
  // consists of the RouteNote data structure (you can check this data
  // structure in the protos/route_guide.proto). The server gets this stream
  // of points and checks if the vector received_notes_ contains already sended
  // points. If so, then the server prepare the stream of this points.
  auto RouteChat() {
    return [this]() {
      return client_->Call<RouteGuide, Stream<RouteNote>, Stream<RouteNote>>(
                 "RouteChat") |
             Client::Handler()
                 .context(std::thread{})
                 // Sending the request of 4 points. We will do it in another
                 // thread.
                 .ready([](auto&& writer, auto& call) {
                   writer = std::thread([&]() {
                     const std::vector<RouteNote> notes{
                         MakeRouteNote("First message", 0, 0),
                         MakeRouteNote("Second message", 0, 1),
                         MakeRouteNote("Third message", 1, 0),
                         MakeRouteNote("Fourth message", 0, 0)};
                     for (const RouteNote& note : notes) {
                       std::cout << "Sending message " << note.message()
                                 << " at " << note.location().latitude()
                                 << ", " << note.location().longitude()
                                 << std::endl;
                       call.Write(note);
                     }
                     call.WritesDone();
                     writer.detach();
                   });
                 })
                 // .body() callback will be invoked every time the server
                 // will send the response.
                 .body([](auto&& writer, auto& call, auto&& server_note) {
                   // We should check if the response is valid from the server
                   if (server_note) {
                     std::cout << "Got message " << server_note->message()
                               << " at " << server_note->location().latitude()
                               << ", " << server_note->location().longitude()
                               << std::endl;
                   }
                 })
                 // .finished callback is invoked when the call is finished.
                 // In this callback we just start our continuation.
                 .finished([](auto&& writer, auto& call, auto&& server_note) {
                   call.Start(grpc::Status::OK);
                 });
    };
  }

 private:
  const float kCoordFactor_ = 10000000.0;
  Client* client_;
  std::vector<Feature> feature_list_;
};

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);

  // Borrowable<T> is data structure which lets us to get borrowed_ptr<T>()
  // that we can pass like a unique_ptr<T> and can give to some part of the
  // code a bunch of things and when they are all done with it, when the last
  // object which points to the data is done with borrowed_ptr, the object
  // which originally created this pointer can find out about that(using
  // watch callback which will tell when the object will no be longer
  // borrowed). More about Borrowable class is in the link below:
  // https://github.com/3rdparty/stout-borrowed-ptr/blob/master/stout/borrowable.h
  Borrowable<CompletionPool> pool;
  Client client("localhost:50051", grpc::InsecureChannelCredentials(),
                pool.Borrow());
  RouteGuideClient guide(db, &client);
  // -------------------------------------------------------------------------
  // -------------------------------------------------------------------------
  std::cout << "-------------- GetFeature --------------" << std::endl;

  // Unary stuff is working
  auto call1 = guide.GetFeature(409146138, -746188906);
  auto call2 = guide.GetFeature(411633782, -746784970);
  auto call3 = guide.GetFeature(0, 0);

  // With stout-eventuals you need to start the calls explicitly. You can use
  // overloaded * operator (implementation in stout-eventuals/src/terminal.h).
  // Remember that this call is blocking!
  auto status = *call1();
  if (!status.ok()) {
    std::cerr << "call1 failed!" << std::endl;
    return EXIT_FAILURE;
  } else {
    std::cout << "call1 successful completed!" << std::endl;
  }

  status = *call2();
  if (!status.ok()) {
    std::cerr << "call2 failed!" << std::endl;
    return EXIT_FAILURE;
  } else {
    std::cout << "call2 successful completed!" << std::endl;
  }

  status = *call3();
  if (!status.ok()) {
    std::cerr << "call3 failed!" << std::endl;
    return EXIT_FAILURE;
  } else {
    std::cout << "call3 successful completed!" << std::endl;
  }
  // -------------------------------------------------------------------------
  // -------------------------------------------------------------------------

  std::cout << "-------------- ListFeatures --------------" << std::endl;
  // Server streaming stuff
  auto call4 = guide.ListFeatures();
  status = *call4();
  if (status.ok()) {
    std::cout << "ListFeatures rpc succeeded." << std::endl;
  } else {
    std::cout << "ListFeatures rpc failed." << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "-------------- RecordRoute --------------" << std::endl;
  // Client streaming stuff
  auto call5 = guide.RecordRoute();
  status = *call5();
  if (status.ok()) {
    std::cout << "RecordRoute rpc was successful." << std::endl;
  } else {
    std::cout << "RecordRoute rpc failed." << std::endl;
  }
  std::cout << "-------------- RouteChat --------------" << std::endl;
  // Bidirectional streaming stuff
  auto call6 = guide.RouteChat();
  status = *call6();
  if (status.ok()) {
    std::cout << "RouteChat rpc was successful." << std::endl;
  } else {
    std::cout << "RouteChat rpc failed." << std::endl;
  }

  return std::cout.good() ? EXIT_SUCCESS : EXIT_FAILURE;
}
