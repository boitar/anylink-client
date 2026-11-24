#!/bin/sh

set -eu

service_label="sslcon"
plist="/Library/LaunchDaemons/sslcon.plist"

if /bin/launchctl print "system/${service_label}" >/dev/null 2>&1; then
    /bin/launchctl bootout "system/${service_label}"
fi

# A previous failed installation can leave the label disabled even after its
# plist is removed. Clear that state before registering the replacement job.
/bin/launchctl enable "system/${service_label}" >/dev/null 2>&1 || true

if [ -f "$plist" ]; then
    /bin/rm -f "$plist"
fi
