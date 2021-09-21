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
#include "stout/terminal.h"

using namespace std::chrono_literals;
using grpc::Status;

using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;

using stout::Borrowable;
using stout::Notification;
//using stout::Sequence;

using stout::eventuals::Terminal;
using stout::eventuals::Terminate;
using stout::eventuals::grpc::Client;
using stout::eventuals::grpc::CompletionPool;
using stout::eventuals::grpc::Stream;

// Populate Point message (you can check this message structure in
// route_guide.proto).
Point MakePoint(long latitude, long longitude) {
  Point p;
  p.set_latitude(latitude);
  p.set_longitude(longitude);
  return p;
}

// Populate Feature message.
Feature MakeFeature(
    const std::string& name,
    long latitude,
    long longitude) {
  Feature f;
  f.set_name(name);
  f.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return f;
}

// Populate RouteNote message.
RouteNote MakeRouteNote(
    const std::string& message,
    long latitude,
    long longitude) {
  RouteNote n;
  n.set_message(message);
  n.mutable_location()->CopyFrom(MakePoint(latitude, longitude));
  return n;
}

// The basic idea of stout-grpc is based on the callbacks. stout-grpc is
// intended to be a higher-level interface while still being asynchronous
// that uses stout-eventuals (https://github.com/3rdparty/stout-eventuals)
// to make composing async code easier.

class RouteGuideClient {
 public:
  // Populate feature_list_ vector with the data from db(route_guide_db.json).
  RouteGuideClient(const std::string& db, Client* client)
    : client_(client) {
    routeguide::ParseDb(db, &feature_list_);
  }

 public:
  // GetFeature demonstrates an async unary rpc call. The client sends the
  // server a request containing a `Point` (latitude, longitude). If the
  // server's database knows that Point, it will respond with the matching
  // address.
  auto GetFeature(long latitude, long longitude) {
    // The idea of Data structure is just to use it in the .context() callback
    // in order to have the way of injecting some data on a stack, or as
    // a part of the eventual data structure and manipulate with this data in
    // multiple callbacks (.start(), .fail(), .ready(), .body(), etc).
    struct Data {
      Point point; // Request from the client.
      Feature feature; // Response from the server.
    };

    // Call() is an `Eventual` (a concept from `stout-eventuals`) and can
    // be composed into an asynchronous pipeline with other Eventuals.
    // In this example we use the "|" operator to compose it with a
    // `Client::Handler()`. This function gets the remote function's name
    // as a parameter. The handler in turn could be composed also.
    // For more information about Eventuals, see:
    // https://github.com/3rdparty/stout-eventuals.
    return client_->Call<RouteGuide, Point, Feature>("GetFeature")
        | (Client::Handler()
               // Passing Data structure to the context.
               .context(Data{MakePoint(latitude, longitude), Feature{}})
               // Send the request with Point's data to the server. First
               // argument in the .ready() callback gets the context. The
               // second argument gets the `call` object, which is how we
               // interact with the running RPC. You can send requests to
               // the server using Write*() family functions. In this
               // case we just send a unique request and finish our call
               // with WriteLast function.
               .ready([](auto&& data, auto& call) {
                 call.WriteLast(data.point);
               })
               // The `.body()` callback is invoked every time the server
               // sends a response. The lambda's first argument gets the
               // context. The second gets the call. The third gets the
               // response object from the server.
               .body([this](auto&& data, auto& call, auto&& response) {
                 // Do not forget to check if the response from the server
                 // is valid! If it is, we process the response. Remember that
                 // all streams end with a final invalid response, even those
                 // that finish correctly!
                 if (response.get() == nullptr) {
                   return;
                 }
                 data.feature.Swap(response.get());
                 if (!data.feature.has_location()) {
                   std::cout << "Server returns incomplete feature."
                             << std::endl;
                   return;
                 }
                 if (data.feature.name().empty()) {
                   std::cout << "Found no feature at "
                             << data.feature.location().latitude()
                           / kCoordFactor_
                             << ", "
                             << data.feature.location().longitude()
                           / kCoordFactor_
                             << std::endl;
                 } else {
                   std::cout << "Found feature called "
                             << data.feature.name() << " at "
                             << data.feature.location().latitude()
                           / kCoordFactor_
                             << ", "
                             << data.feature.location().longitude()
                           / kCoordFactor_
                             << std::endl;
                 }
               })
               // .finished callback is invoked when the call is finished.
               // In this callback we just start our continuation.
               .finished([](
                             auto&& data,
                             auto& continuation,
                             auto&& resp) {
                 continuation.Start(grpc::Status::OK);
               }));
  }

  // The `ListFeatures()` function demonstrates a streaming RPC. It is
  // comparable to `GetFeature()`, but instead of sending a single `Point`
  // the client sends a `Rectangle`. The server responds with a stream of
  // messages containing addresses that fall inside the Rectangle's area.
  auto ListFeatures() {
    struct Data {
      Rectangle rectangle; // Request from the client.
      Feature feature; // Response from the server.
    };
    return client_->Call<RouteGuide, Rectangle, Stream<Feature>>(
               "ListFeatures")
        | Client::Handler()
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
              // The .body() callback is invoked every time the server
              // sends a response. The response contains the addresses
              // that fall within the requested rectangle's area.
              .body([this](auto&& data, auto& call, auto&& feature) {
                // Check if the response from the server is valid.
                if (feature.get() == nullptr) {
                  return;
                }
                std::cout
                    << "Found feature called " << feature->name()
                    << " at "
                    << feature->location().latitude() / kCoordFactor_
                    << ", "
                    << feature->location().longitude() / kCoordFactor_
                    << std::endl;
              })
              // .finished callback is invoked when the call is finished.
              // In this callback we just start our continuation.
              .finished([](
                            auto&& data,
                            auto& continuation,
                            auto&& response) {
                continuation.Start(grpc::Status::OK);
              });
  }

  // RecordRoute() demonstrates a client streaming RPC. The client sends 10
  // Point messages to the server. The server responds with a single message
  // which contains the number of points, number of features found in the db,
  // the distance calculated from all Point coordinates, and the resulting
  // time.
  auto RecordRoute() {
    struct Data {
      size_t num_points; // The number of points to send to the server.
      Point point; // The request to the server.
      RouteSummary stats; // The response from the server.
    };

    return client_->Call<RouteGuide, Stream<Point>, RouteSummary>(
               "RecordRoute")
        | Client::Handler()
              .context(Data{10, Point{}, RouteSummary{}})
              // Prepare 10 points for the request by randomly choosing
              // 10 points from the db. After sending every request there
              // is also a little delay in order to simulate "long" travelling
              // from one point to another.
              .ready([this](auto&& data, auto& call) {
                unsigned seed = std::chrono::system_clock::now()
                                    .time_since_epoch()
                                    .count();

                std::default_random_engine generator(seed);
                std::uniform_int_distribution<int> feature_distribution(
                    0,
                    feature_list_.size() - 1);
                std::uniform_int_distribution<int> delay_distribution(
                    500,
                    1500);

                for (int i = 0; i < data.num_points; i++) {
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

              // The .body() callback is invoked every time the server
              // sends a response.
              .body([](auto&& data, auto& call, auto&& response) {
                // We should check if the response is valid from the server.
                if (response.get() == nullptr) {
                  return;
                }
                data.stats.Swap(response.get());
                std::cout << "Finished trip with "
                          << data.stats.point_count() << " points\n"
                          << "Passed " << data.stats.feature_count()
                          << " features\n"
                          << "Travelled " << data.stats.distance()
                          << " meters\n"
                          << "It took " << data.stats.elapsed_time()
                          << " seconds" << std::endl;
              })
              // .finished callback is invoked when the call is finished.
              // In this callback we just start our continuation.
              .finished([](
                            auto&& data,
                            auto& continuation,
                            auto&& response) {
                continuation.Start(grpc::Status::OK);
              });
  }

  // RouteChat() demonstrates a bidirectional streaming RPC. The client
  // sends a sequence of 4 messages. Each message consists of a RouteNote
  // (see protos/route_guide.proto), which contains a location. The server
  // streams back any notes it has previously received for those locations.
  auto RouteChat() {
    return client_->Call<
               RouteGuide,
               Stream<RouteNote>,
               Stream<RouteNote>>("RouteChat")
        | Client::Handler()
              .context(std::thread{})
              // Send a stream of 4 points. We do that in a separate thread to
              // be able to receive messages simultaneously.
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
                    std::this_thread::sleep_for(100ms);
                  }
                  call.WritesDone();
                });
                writer.detach();
              })
              // The .body() callback is invoked every time the server
              // sends a response.
              .body([](
                        auto&& writer,
                        auto& call,
                        auto&& server_note) {
                // We should check if the response is valid from the server.
                if (server_note.get() == nullptr) {
                  return;
                }
                std::cout << "Got message " << server_note->message()
                          << " at " << server_note->location().latitude()
                          << ", " << server_note->location().longitude()
                          << std::endl;
              })
              // .finished callback is invoked when the call is finished.
              // In this callback we just start our continuation.
              .finished([](
                            auto&& writer,
                            auto& continuation,
                            auto&& server_note) {
                continuation.Start(grpc::Status::OK);
              });
  }

 private:
  const float kCoordFactor_ = 10000000.0;
  Client* client_;
  std::vector<Feature> feature_list_;
};

int main(int argc, char** argv) {
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  std::string db = routeguide::GetDbFileContent(argc, argv);

  // `Borrowable<T>` is data structure that constructs a `borrowed_ptr<T>`.
  // Borrowed pointers are smart pointers that we can pass like a
  // `unique_ptr<T>`, and then be notified when the last object that points to
  // the data is done with it. We learn that a `borrowed_ptr` is no longer
  // being used from its `Watch()` callback. More about the Borrowable class is
  // here:
  // https://github.com/3rdparty/stout-borrowed-ptr/blob/master/stout/borrowable.h
  Borrowable<CompletionPool> pool;
  Client client(
      "localhost:50051",
      grpc::InsecureChannelCredentials(),
      pool.Borrow());
  RouteGuideClient guide(db, &client);
  // -------------------------------------------------------------------------
  // -------------------------------------------------------------------------
  std::cout << "-------------- GetFeature --------------" << std::endl;

  // With stout-eventuals you need to start the calls explicitly. You can use
  // overloaded * operator (implementation in stout-eventuals/src/terminal.h).
  // Remember that this call is blocking!

  // Demonstrate unary (single-request, single-response) RPCs.
  auto status = *(guide.GetFeature(409146138, -746188906));
  if (!status.ok()) {
    std::cerr << "GetFeature failed!" << std::endl;
    return EXIT_FAILURE;
  } else {
    std::cout << "GetFeature successful completed!" << std::endl;
  }

  status = *(guide.GetFeature(411633782, -746784970));
  if (!status.ok()) {
    std::cerr << "GetFeature failed!" << std::endl;
    return EXIT_FAILURE;
  } else {
    std::cout << "GetFeature successful completed!" << std::endl;
  }

  status = *(guide.GetFeature(0, 0));
  if (!status.ok()) {
    std::cerr << "GetFeature failed!" << std::endl;
    return EXIT_FAILURE;
  } else {
    std::cout << "GetFeature successful completed!" << std::endl;
  }
  // -------------------------------------------------------------------------
  // -------------------------------------------------------------------------

  std::cout << "-------------- ListFeatures --------------" << std::endl;
  // Demonstrate server streaming (single-request, multiple-responses) RPC.
  status = *(guide.ListFeatures());
  if (status.ok()) {
    std::cout << "ListFeatures rpc succeeded." << std::endl;
  } else {
    std::cout << "ListFeatures rpc failed." << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "-------------- RecordRoute --------------" << std::endl;
  // Demonstrate client streaming (multiple-requests, single-response) RPC.
  status = *(guide.RecordRoute());
  if (status.ok()) {
    std::cout << "RecordRoute rpc was successful." << std::endl;
  } else {
    std::cout << "RecordRoute rpc failed." << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "-------------- RouteChat --------------" << std::endl;
  // Demonstrate bidirectional streaming (multiple-requests, multiple-response)
  // RPC.
  status = *(guide.RouteChat());
  if (status.ok()) {
    std::cout << "RouteChat rpc was successful." << std::endl;
  } else {
    std::cout << "RouteChat rpc failed." << std::endl;
    return EXIT_FAILURE;
  }

  return std::cout.good() ? EXIT_SUCCESS : EXIT_FAILURE;
}
