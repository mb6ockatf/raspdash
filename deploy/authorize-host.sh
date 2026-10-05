#!/bin/sh
# authorize-host.sh -- let the dashboard Pi read a machine's statistics.
#
# Copyright (C) 2026 mb6ockatf
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Usage: deploy/authorize-host.sh [-n] [-k KEYFILE] [-f FROM] USER@HOST
#
# Logs in to USER@HOST once (it asks for the password there), installs
# agent/raspdash-stats.sh as ~/.local/bin/raspdash-stats and appends the
# Pi's public key to ~/.ssh/authorized_keys, restricted to running only
# that script, without a terminal or forwarding, and only from FROM.
# Running it again replaces the script and leaves the key line alone.
#
#   -k KEYFILE  public key to authorize (default: fetched with
#               'ssh pi cat ~/.ssh/raspdash_ed25519.pub')
#   -f FROM     address the Pi connects from (default: 192.168.1.96)
#   -n          print the commands that would run on USER@HOST and exit

set -eu

here=$(cd "$(dirname "$0")" && pwd)
stats=$here/../agent/raspdash-stats.sh
keyfile=
from=192.168.1.96
dry_run=0

while getopts nk:f: opt; do
    case $opt in
        n) dry_run=1 ;;
        k) keyfile=$OPTARG ;;
        f) from=$OPTARG ;;
        *) sed -n '11,23s/^# \{0,1\}//p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { sed -n '11,23s/^# \{0,1\}//p' "$0" >&2; exit 2; }
target=$1

if [ -n "$keyfile" ]; then
    key=$(cat "$keyfile")
else
    key=$(ssh pi cat .ssh/raspdash_ed25519.pub)
fi
case $key in
    ssh-*|ecdsa-*) ;;
    *) echo "authorize-host: '$key' does not look like a public key" >&2
       exit 1 ;;
esac

remote_script() {
    cat <<EOF
set -eu
umask 077
mkdir -p "\$HOME/.local/bin" "\$HOME/.ssh"
cat >"\$HOME/.local/bin/raspdash-stats" <<'RASPDASH_STATS_EOF'
EOF
    cat "$stats"
    cat <<EOF
RASPDASH_STATS_EOF
chmod 755 "\$HOME/.local/bin/raspdash-stats"
keys=\$HOME/.ssh/authorized_keys
touch "\$keys"
chmod 600 "\$keys"
key='$key'
if grep -qF "\$key" "\$keys"; then
    echo "key already authorized on \$(hostname)"
else
    cp "\$keys" "\$keys.bak-raspdash"
    printf 'command="%s",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding,from="%s" %s\n' \\
        "\$HOME/.local/bin/raspdash-stats" '$from' "\$key" >>"\$keys"
    echo "key authorized on \$(hostname)"
fi
"\$HOME/.local/bin/raspdash-stats"
EOF
}

if [ "$dry_run" = 1 ]; then
    remote_script
    exit 0
fi

remote_script | ssh -o StrictHostKeyChecking=accept-new "$target" 'sh -s'

echo "Checking from the Pi..."
ssh pi "ssh -i ~/.ssh/raspdash_ed25519 -o BatchMode=yes \
    -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 \
    ${target} true"
