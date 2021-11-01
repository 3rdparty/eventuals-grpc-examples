# gRPC Examples using `eventuals::grpc`

This repository provides [gRPC](https://grpc.io) examples written using [`eventuals::grpc`](https://github.com/3rdparty/eventuals-grpc).

These examples are in a separate repository from `eventuals::grpc` to make it easier to clone that repository and start building a project rather than trying to figure out what pieces of the `eventuals::grpc` build should be copied.

**Please see the `eventuals::grpc` [README](https://github.com/3rdparty/eventuals-grpc) for more information on the `eventuals::grpc` library!**

## RouteGuide

The RouteGuide example can be found in [route_guide](https://github.com/3rdparty/eventuals-grpc-examples/tree/master/route_guide). Build the server and client with:

```sh
$ bazel build :route_guide_server
...
$ bazel build :route_guide_client
...
```