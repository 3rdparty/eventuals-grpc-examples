"""Dependency specific initialization."""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

load("@com_github_3rdparty_eventuals_grpc//bazel:deps.bzl", eventuals_grpc_deps="deps")

def deps(repo_mapping = {}):
    eventuals_grpc_deps(
        repo_mapping = repo_mapping,
    )
