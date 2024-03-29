"""Adds repostories/archives."""

########################################################################
# DO NOT EDIT THIS FILE unless you are inside the
# https://github.com/3rdparty/stout-notification repository. If you
# encounter it anywhere else it is because it has been copied there in
# order to simplify adding transitive dependencies. If you want a
# different version of stout-notification follow the Bazel build
# instructions at https://github.com/3rdparty/stout-notification.
########################################################################

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

def repos(external = True, repo_mapping = {}):
    if external and "com_github_3rdparty_stout_notification" not in native.existing_rules():
        git_repository(
            name = "com_github_3rdparty_stout_notification",
            remote = "https://github.com/3rdparty/stout-notification",
            commit = "73debaa137aa3932fc64881dc6183e64e822f737",
            shallow_since = "1629613922 +0200",
            repo_mapping = repo_mapping,
        )
