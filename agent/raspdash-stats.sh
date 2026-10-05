#!/bin/sh
# raspdash-stats -- print system statistics as key=value lines.
#
# Copyright (C) 2026 mb6ockatf
#
# This file is part of raspdash.
#
# raspdash is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# raspdash is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with raspdash.  If not, see <https://www.gnu.org/licenses/>.
#
# raspdash pipes this script to 'sh -s' over ssh, or runs it locally.
# It can also be installed on a host as the forced command of a
# restricted key in ~/.ssh/authorized_keys.  It needs nothing beyond a
# POSIX shell and the base system on Linux and macOS.
#
# Output (all optional except agent):
#   agent=1  hostname= os= kernel= uptime_s= ncpu=
#   load1= load5= load15= cpu_pct=
#   mem_total= mem_avail= swap_total= swap_used=     (bytes)
#   disk_total= disk_used=                           (bytes, root fs)
#   temp.<chip>.<label>=<celsius>
#   battery_pct= battery_status=

LC_ALL=C
export LC_ALL
PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin:$PATH

echo agent=1

# Replace characters that are awkward in keys.
key () {
    printf '%s' "$1" | tr -c 'A-Za-z0-9_+-' '_'
}

disk_root () {
    # df -P -k: 1024-byte blocks, one line per filesystem.
    df -P -k "$1" 2>/dev/null | awk 'NR == 2 {
        printf "disk_total=%.0f\ndisk_used=%.0f\n", $2 * 1024, $3 * 1024 }'
}

linux () {
    printf 'hostname=%s\n' "$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname)"
    if [ -r /etc/os-release ]; then
        os=$(. /etc/os-release && printf '%s' "${PRETTY_NAME:-$NAME}")
    else
        os=Linux
    fi
    printf 'os=%s\n' "$os"
    printf 'kernel=%s\n' "$(uname -r)"
    awk '{ printf "uptime_s=%d\n", $1 }' /proc/uptime
    printf 'ncpu=%s\n' "$(getconf _NPROCESSORS_ONLN 2>/dev/null \
        || grep -c '^processor' /proc/cpuinfo)"
    awk '{ printf "load1=%s\nload5=%s\nload15=%s\n", $1, $2, $3 }' /proc/loadavg

    # CPU busy share between two samples of the aggregate "cpu" line.
    s1=$(head -n 1 /proc/stat)
    sleep 0.5 2>/dev/null || sleep 1
    s2=$(head -n 1 /proc/stat)
    printf '%s\n%s\n' "$s1" "$s2" | awk '
        { idle = $5 + $6; total = 0
          for (i = 2; i <= NF; i++) total += $i
          if (NR == 1) { i1 = idle; t1 = total } else { i2 = idle; t2 = total } }
        END { dt = t2 - t1
              if (dt > 0) printf "cpu_pct=%.1f\n", 100 * (dt - (i2 - i1)) / dt }'

    awk '
        /^MemTotal:/     { t = $2 }
        /^MemAvailable:/ { a = $2 }
        /^MemFree:/      { f = $2 }
        /^Buffers:/      { b = $2 }
        /^Cached:/       { c = $2 }
        /^SwapTotal:/    { st = $2 }
        /^SwapFree:/     { sf = $2 }
        END { if (a == "") a = f + b + c
              printf "mem_total=%.0f\nmem_avail=%.0f\n", t * 1024, a * 1024
              printf "swap_total=%.0f\nswap_used=%.0f\n", st * 1024, (st - sf) * 1024 }
    ' /proc/meminfo

    disk_root /

    found=
    for h in /sys/class/hwmon/hwmon*; do
        [ -r "$h/name" ] || continue
        chip=$(key "$(cat "$h/name")")
        for t in "$h"/temp*_input; do
            [ -r "$t" ] || continue
            v=$(cat "$t" 2>/dev/null) || continue
            [ -n "$v" ] || continue
            l=${t%_input}_label
            if [ -r "$l" ]; then
                label=$(key "$(cat "$l")")
            else
                label=$(basename "${t%_input}")
            fi
            printf 'temp.%s.%s=%s\n' "$chip" "$label" \
                "$(awk -v v="$v" 'BEGIN { printf "%.1f", v / 1000 }')"
            found=1
        done
    done
    if [ -z "$found" ]; then
        for z in /sys/class/thermal/thermal_zone*; do
            [ -r "$z/temp" ] || continue
            v=$(cat "$z/temp" 2>/dev/null) || continue
            printf 'temp.%s.%s=%s\n' "$(key "$(cat "$z/type")")" \
                "$(basename "$z")" \
                "$(awk -v v="$v" 'BEGIN { printf "%.1f", v / 1000 }')"
        done
    fi

    for b in /sys/class/power_supply/BAT*; do
        [ -r "$b/capacity" ] || continue
        printf 'battery_pct=%s\n' "$(cat "$b/capacity")"
        [ -r "$b/status" ] && printf 'battery_status=%s\n' "$(cat "$b/status")"
        break
    done
}

darwin () {
    # A forced-command ssh login gets a minimal PATH without Homebrew.
    PATH=$PATH:/opt/homebrew/bin:/usr/local/bin

    printf 'hostname=%s\n' "$(hostname -s)"
    printf 'os=macOS %s\n' "$(sw_vers -productVersion 2>/dev/null)"
    printf 'kernel=%s\n' "$(uname -r)"
    boot=$(sysctl -n kern.boottime | sed -n 's/^{ *sec = \([0-9]*\),.*/\1/p')
    [ -n "$boot" ] && printf 'uptime_s=%s\n' "$(( $(date +%s) - boot ))"
    printf 'ncpu=%s\n' "$(sysctl -n hw.ncpu)"
    sysctl -n vm.loadavg | awk '{ printf "load1=%s\nload5=%s\nload15=%s\n", $2, $3, $4 }'

    # The second sample of top(1) covers the last second.
    top -l 2 -n 0 -s 1 2>/dev/null | awk '/^CPU usage/ { l = $0 }
        END { if (match (l, /[0-9.]+% idle/)) {
                idle = substr (l, RSTART, RLENGTH - 6)
                printf "cpu_pct=%.1f\n", 100 - idle } }'

    printf 'mem_total=%s\n' "$(sysctl -n hw.memsize)"
    vm_stat | awk '
        /page size of/        { ps = $8 }
        /^Pages free/         { f = $3 }
        /^Pages inactive/     { i = $3 }
        /^Pages speculative/  { s = $3 }
        /^Pages purgeable/    { p = $3 }
        END { printf "mem_avail=%.0f\n", (f + i + s + p) * ps }'
    sysctl -n vm.swapusage | awk '
        function to_bytes(v) { u = substr (v, length (v)); n = v + 0
            return n * (u == "G" ? 1073741824 : u == "M" ? 1048576 : u == "K" ? 1024 : 1) }
        { printf "swap_total=%.0f\nswap_used=%.0f\n", to_bytes($3), to_bytes($6) }'

    # / is the sealed system volume; user data lives on the Data volume.
    if [ -d /System/Volumes/Data ]; then
        disk_root /System/Volumes/Data
    else
        disk_root /
    fi

    # Temperatures need a third-party tool on macOS.
    if command -v smctemp >/dev/null 2>&1; then
        v=$(smctemp -c 2>/dev/null) && [ -n "$v" ] && printf 'temp.smc.cpu=%s\n' "$v"
        v=$(smctemp -g 2>/dev/null) && [ -n "$v" ] && printf 'temp.smc.gpu=%s\n' "$v"
    elif command -v osx-cpu-temp >/dev/null 2>&1; then
        v=$(osx-cpu-temp 2>/dev/null | sed 's/[^0-9.].*//')
        [ -n "$v" ] && printf 'temp.smc.cpu=%s\n' "$v"
    fi

    pmset -g batt 2>/dev/null | awk -F'\t' '/InternalBattery/ {
        split ($2, f, ";"); pct = f[1]; sub (/%/, "", pct)
        st = f[2]; sub (/^ */, "", st)
        printf "battery_pct=%s\nbattery_status=%s\n", pct, st }'
}

case $(uname -s) in
    Linux)  linux ;;
    Darwin) darwin ;;
    *)      printf 'hostname=%s\nos=%s\n' "$(hostname)" "$(uname -s)" ;;
esac
exit 0
