"""Adds repositories/archives"""

########################################################################
# DO NOT EDIT THIS FILE unless you are inside the
# https://github.com/3rdparty/eventuals repository. If you
# encounter it anywhere else it is because it has been copied there in
# order to simplify adding transitive dependencies. If you want a
# different version of eventuals follow the Bazel build
# instructions at https://github.com/3rdparty/eventuals.
########################################################################

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("//3rdparty/bazel-rules-curl:repos.bzl", curl_repos = "repos")
load("//3rdparty/bazel-rules-jemalloc:repos.bzl", jemalloc_repos = "repos")
load("//3rdparty/bazel-rules-libuv:repos.bzl", libuv_repos = "repos")

def repos(external = True, repo_mapping = {}):
    """Adds repositories/archives needed by eventuals

    Args:
          external: whether or not we're invoking this function as though
            though we're an external dependency
          repo_mapping: passed through to all other functions that expect/use
            repo_mapping, e.g., 'git_repository'
    """

    libuv_repos(
        repo_mapping = repo_mapping,
    )

    curl_repos(
        repo_mapping = repo_mapping,
    )

    jemalloc_repos(
        repo_mapping = repo_mapping,
    )

    if external:
        maybe(
            git_repository,
            name = "com_github_3rdparty_eventuals",
            remote = "https://github.com/3rdparty/eventuals",
            commit = "303bb6a5fe3544f3f5a05b4733d3efc84de3c601",
            shallow_since = "1636402389 -0800",
            repo_mapping = repo_mapping,
        )
