"""Adds repostories/archives."""

########################################################################
# DO NOT EDIT THIS FILE unless you are inside the
# https://github.com/3rdparty/stout-borrowed-ptr repository. If you
# encounter it anywhere else it is because it has been copied there in
# order to simplify adding transitive dependencies. If you want a
# different version of stout-borrowed-ptr follow the Bazel build
# instructions at https://github.com/3rdparty/stout-borrowed-ptr.
########################################################################

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

load("//3rdparty/stout-stateful-tally:repos.bzl", stout_stateful_tally_repos="repos")

def repos(external = True, repo_mapping = {}):
    stout_stateful_tally_repos()
    
    if external and "com_github_3rdparty_stout_borrowed_ptr" not in native.existing_rules():
        git_repository(
            name = "com_github_3rdparty_stout_borrowed_ptr",
            remote = "https://github.com/3rdparty/stout-borrowed-ptr",
            commit = "1217f2fed7dc805555b0145fabf6c23b00813160",
            shallow_since = "1629613454 +0200",
            repo_mapping = repo_mapping,
        )
