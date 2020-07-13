# gRPC Examples using `stout::grpc`

This repository provides [gRPC](https://grpc.io) examples written using [`stout::grpc`](https://github.com/3rdparty/stout-grpc).

These examples are in a separate repository from `stout::grpc` to make it easier to clone that repository and start building a project rather than trying to figure out what pieces of the `stout::grpc` build should be copied.

**Please see the `stout::grpc` [README](https://github.com/3rdparty/stout-grpc) for more information on the `stout::grpc` library!**

## RouteGuide

The RouteGuide example can be found in [route_guide](https://github.com/3rdparty/stout-grpc-examples/tree/master/route_guide). Build the server and client with:

```sh
$ bazel build :route_guide_server
...
$ bazel build :route_guide_client
...
```