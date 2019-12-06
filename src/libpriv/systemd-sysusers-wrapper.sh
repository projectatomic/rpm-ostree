#!/usr/bin/bash
# Used by rpmostree-core.c to intercept `systemd-sysusers` invocations
# that are done inline; we need the invocations to see our fixed uid/gid
# mappings.  So we will ensure the ones from disk are used instead.

echo "rpm-ostree: Ignoring systemd-sysusers invocation"
