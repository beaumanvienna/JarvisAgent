#!/usr/bin/env bash
# @jarvis-script
# @short: Extract structured OpenSSH attack stats from logs
# @params: input_log output_json
# @description: Parses OpenSSH auth logs and outputs per-IP attack stats, including attempt counts, usernames, time windows, and success/failure ratios as JSON.
# @outputs: Structured JSON stats to output_json

set -euo pipefail

# Assign positional args (never hardcode paths):
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 input_log output_json" >&2
  exit 1
fi

infile="$1"
outfile="$2"

# Check input file exists
if [[ ! -f "$infile" ]]; then
  echo "Error: input_log '$infile' does not exist." >&2
  exit 2
fi

# Temporary files for sorting/intermediate output
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Extract relevant lines and canonicalize whitespace for parsing
# Accepts logs from either /var/log/auth.log or /var/log/secure style

# For timezone coherence, we pass epoch timestamps through date -d (POSIX, but on Zorin OS 18 GNU date is default).

# awk: gather per-IP stats into tabular data
awk '
function min(a, b) {
  return a < b ? a : b
}
function max(a, b) {
  return a > b ? a : b
}
# ORDERS: Month Day Time Host sshd[PID]: Type User from IP port ...
# Example failed: Jun  7 23:45:11 server sshd[28276]: Failed password for root from 93.174.93.65 port 51348 ssh2
# Example success: Jun  7 23:45:12 server sshd[28276]: Accepted password for admin from 185.6.233.152 port 2205 ssh2

BEGIN {
  # Map month shortname to month number
  m["Jan"]="01"; m["Feb"]="02"; m["Mar"]="03"; m["Apr"]="04";
  m["May"]="05"; m["Jun"]="06"; m["Jul"]="07"; m["Aug"]="08";
  m["Sep"]="09"; m["Oct"]="10"; m["Nov"]="11"; m["Dec"]="12";
}
{
  # Collapse multiple spaces and parse fields 1-3 as date, then shift
  line = $0
  gsub(/[ ]+/, " ", line)
  split(line, f, " ")

  # Date fields
  month=f[1]; day=f[2]; time=f[3]
  # Host is f[4], main message after f[5]:
  msg = ""
  for (i=6; i<=length(f); i++) {
    msg = msg f[i]
    if (i<length(f)) msg = msg " "
  }

  is_failed=0
  is_accept=0
  is_invalid=0
  
  is_pubkey=0
  is_root=0
  user=""
  ip=""
  found=0

  # Parse for Failed/Accepted/Invalid
  # Split msg once; use field positions for each pattern
  n_mf = split(msg, mf, " ")

  # "Failed password for invalid user <USER> from <IP> ..."
  if (match(msg, /^Failed password for invalid user [^ ]+ from /)) {
    is_failed=1; is_invalid=1; found=1
    user = mf[6]; ip = mf[8]
  # "Failed password for <USER> from <IP> ..."
  } else if (match(msg, /^Failed password for [^ ]+ from /)) {
    is_failed=1; found=1
    user = mf[4]; ip = mf[6]
  # "Accepted password for <USER> from <IP> ..."
  } else if (match(msg, /^Accepted password for [^ ]+ from /)) {
    is_accept=1; found=1
    user = mf[4]; ip = mf[6]
  # "Invalid user <USER> from <IP>"
  } else if (match(msg, /^Invalid user [^ ]+ from /)) {
    is_invalid=1; found=1
    user = mf[3]; ip = mf[5]
  # "Accepted publickey for <USER> from <IP> ..."
  } else if (match(msg, /^Accepted publickey for [^ ]+ from /)) {
    is_accept=1; is_pubkey=1; found=1
    user = mf[4]; ip = mf[6]
  # "Failed publickey for <USER> from <IP> ..."
  } else if (match(msg, /^Failed publickey for [^ ]+ from /)) {
    is_failed=1; is_pubkey=1; found=1
    user = mf[4]; ip = mf[6]
  }

  if (found && ip ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/) {
    # Compose timestamp
    # Fill in year (last data point in logs probably this year)
    # We guess year as current year (better variant would detect from log, out of scope)
    cmd="date +%Y"
    cmd | getline year
    close(cmd)
    dt=year"-"m[month]"-"sprintf("%02d",day)"T"time
    gsub(/\./,":",dt)
    # dt: 2024-06-08T01:23:45
    ep=""
    cmd="date -d \""month" "day" "time"\" +%s"
    cmd | getline ep
    close(cmd)

    key = ip SUBSEP user
    attempts[key] += 1

    # First/last seen per IP
    if (!(ip in first_seen) || ep < first_seen[ip]) first_seen[ip] = ep
    if (!(ip in last_seen) || ep > last_seen[ip]) last_seen[ip] = ep

    # Track usernames per IP
    if (!(u_seen[ip,user])) {
      userlist[ip] = (ip in userlist ? userlist[ip] "," : "") user
      u_seen[ip,user]=1
    }

    # Overall counts
    ips[ip]=1
    usernames[user]=1

    if (is_failed) {faileds[ip]++; failedu[ip,user]++;}
    if (is_accept) {accepts[ip]++; acceptu[ip,user]++;}
    if (is_invalid) {invalids[ip]++; invalidu[ip,user]++;}
    if (is_pubkey) {pubkey[ip]++;}
    if (user=="root") {root[ip]++; if(is_failed) rootfail[ip]++; if(is_accept) rootsucc[ip]++;}
  }
}
END {
  # Print tabular summary, tab-separated:
  # IP\tUser\tAttempts\tFails\tAccepts\tInvalids\tFirstSeen\tLastSeen\tRoot\tPubkey
  for (k in attempts) {
    split(k, parts, SUBSEP)
    ip=parts[1]; user=parts[2]
    n_attempt=attempts[k]
    n_fail=(ip in faileds ? failedu[ip,user]+0 : 0)
    n_accept=(ip in accepts ? acceptu[ip,user]+0 : 0)
    n_invalid=(ip in invalids ? invalidu[ip,user]+0 : 0)
    n_pubkey=(ip in pubkey ? pubkey[ip]+0 : 0)
    n_root=(ip in root ? root[ip]+0 : 0)
    n_rootfail=(ip in rootfail ? rootfail[ip]+0 : 0)
    n_rootsucc=(ip in rootsucc ? rootsucc[ip]+0 : 0)
    first=first_seen[ip]
    last=last_seen[ip]
    printf "%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\n", ip, user, n_attempt, n_fail, n_accept, n_invalid, first, last, n_root, n_pubkey
  }
}
' "$infile" > "$tmpdir/stats_tab.txt"

# Collect overall lists
awk -F"\t" '
{
  ips[$1]=1; users[$2]=1
  fail[$1]+=$4; accept[$1]+=$5; atts[$1]+=$3
  userips[$2]++
}
END {
  sep=""
  printf "{"
  printf "\"ips\":["
  for (ip in ips) {printf "%s\"%s\"",sep,ip;sep=","}
  printf "],"
  sep=""
  printf "\"usernames\":["
  for (u in users) {printf "%s\"%s\"",sep,u;sep=","}
  printf "],"
  sep=""
  printf "\"user_counts\":{"
  for(u in userips){printf "%s\"%s\":%d",sep,u,userips[u];sep=","}
  printf "},"
  sep=""
  printf "\"summary\":{"
  for(ip in ips){printf "%s\"%s\":{\"attempts\":%d,\"fails\":%d,\"accepts\":%d}",sep,ip,atts[ip],fail[ip],accept[ip];sep=","}
  printf "}"
  printf "}\n"
}
' "$tmpdir/stats_tab.txt" > "$tmpdir/meta.json"

# Generate detailed per-IP attack stats as flat CSV
awk -F"\t" '
{
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5, $6, $7, $8, $9, $10
}' "$tmpdir/stats_tab.txt" > "$tmpdir/ip_user_rows.tsv"

# Now aggregate per IP attack profile into JSON using POSIX shell and jq

# Build JSON array of attack profiles
awk -F"\t" '
{
  ip=$1; user=$2
  arr[ip,user,"attempts"]=$3
  arr[ip,user,"fails"]=$4
  arr[ip,user,"accepts"]=$5
  arr[ip,user,"invalids"]=$6
  arr[ip,user,"first"]=$7
  arr[ip,user,"last"]=$8
  arr[ip,user,"root"]=$9
  arr[ip,user,"pubkey"]=$10
  ips[ip]=1
  users[user]=1
}
END {
  # Per IP: collect all users, time window
  printf "["
  s=""
  for (ip in ips) {
    # Find time range for IP (min first, max last)
    minfirst="9999999999"
    maxlast="0"
    u=""
    n_users=0
    ua=""
    for (user in users) {
      k0=ip SUBSEP user SUBSEP "attempts"
      if (k0 in arr) {
        first=arr[ip,user,"first"]
        last=arr[ip,user,"last"]
        if (first < minfirst) minfirst=first
        if (last > maxlast) maxlast=last
        n_users++
      }
    }
    # User entries per IP
    printf "%s{\"ip\":\"%s\",\"first_seen\":%s,\"last_seen\":%s,", s, ip, minfirst, maxlast
    printf "\"attackers\":["
    us=""
    for (user in users) {
      k0=ip SUBSEP user SUBSEP "attempts"
      if (k0 in arr) {
        printf "%s{\"username\":\"%s\",\"attempts\":%s,\"fails\":%s,\"accepts\":%s,\"invalids\":%s,\"root\":%s,\"pubkey\":%s,\"first_seen\":%s,\"last_seen\":%s}", us, user, arr[ip,user,"attempts"], arr[ip,user,"fails"], arr[ip,user,"accepts"], arr[ip,user,"invalids"], arr[ip,user,"root"], arr[ip,user,"pubkey"], arr[ip,user,"first"], arr[ip,user,"last"]
        us=","
      }
    }
    printf "]}"
    s=","
  }
  printf "]"
}
' "$tmpdir/ip_user_rows.tsv" > "$tmpdir/ip_profiles.json"

# Combine meta and per IP profiles into full JSON
jq -s 'def merge2(a;b): a * b;
  (.[0] + {"attack_profiles": .[1]})' \
  "$tmpdir/meta.json" "$tmpdir/ip_profiles.json" > "$outfile"
