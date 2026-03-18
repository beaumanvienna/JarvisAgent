#!/usr/bin/env python3
# @jarvis-script
# @short: Parse OpenSSH logs to extract structured attack statistics
# @description: Parses raw sshd log files to extract per-IP attack profiles including attempt counts, targeted usernames, time windows, port sequences, username enumeration patterns, reverse DNS anomalies, and success vs failure login ratios; outputs JSON summary.
# @outputs: attack_stats.json

import os
import re
import json
from collections import defaultdict
from datetime import datetime

def extract_attack_statistics(context=None, **kwargs):
    log_path = context['_file_input_0']
    workdir = context['_task_working_directory']
    out_path = os.path.join(workdir, "attack_stats.json")

    # Regex patterns
    # Example log line formats (varies by sshd config):
    # Jun 10 06:35:27 hostname sshd[1234]: Failed password for invalid user admin from 192.0.2.1 port 39201 ssh2
    # Jun 10 06:36:00 hostname sshd[1234]: Accepted password for root from 203.0.113.5 port 22 ssh2
    # Jun 10 06:36:22 hostname sshd[1234]: Invalid user test from 198.51.100.7 port 34567
    # To parse month day time, e.g. "Jun 10 06:35:27"
    month_map = {'Jan':1,'Feb':2,'Mar':3,'Apr':4,'May':5,'Jun':6,'Jul':7,'Aug':8,'Sep':9,'Oct':10,'Nov':11,'Dec':12}

    # Build datetime per entry, year not given. Use current year. If log includes entries spanning year change, might be wrong.
    current_year = datetime.now().year

    # Patterns for extracting fields
    # Fields of interest: timestamp, IP, username, success/fail, port, reverse dns anomaly, etc.
    re_failed_pass = re.compile(
        r"^(?P<month>\w{3}) +(?P<day>\d{1,2}) (?P<time>\d{2}:\d{2}:\d{2}) [\w\-.]+ sshd\[\d+\]: Failed password for (invalid user )?(?P<user>\S+) from (?P<ip>\d{1,3}(?:\.\d{1,3}){3}) port (?P<port>\d+) ssh2"
    )
    re_accepted_pass = re.compile(
        r"^(?P<month>\w{3}) +(?P<day>\d{1,2}) (?P<time>\d{2}:\d{2}:\d{2}) [\w\-.]+ sshd\[\d+\]: Accepted password for (?P<user>\S+) from (?P<ip>\d{1,3}(?:\.\d{1,3}){3}) port (?P<port>\d+) ssh2"
    )
    re_invalid_user = re.compile(
        r"^(?P<month>\w{3}) +(?P<day>\d{1,2}) (?P<time>\d{2}:\d{2}:\d{2}) [\w\-.]+ sshd\[\d+\]: Invalid user (?P<user>\S+) from (?P<ip>\d{1,3}(?:\.\d{1,3}){3}) port (?P<port>\d+)"
    )
    re_reverse_dns_fail = re.compile(
        r"reverse mapping checking getaddrinfo for (?P<ip>\d{1,3}(?:\.\d{1,3}){3}) \[(?P<hostname>[^\]]*)\] failed"
    )

    # Data structures
    ip_profiles = defaultdict(lambda: {
        'attempts': 0,
        'failed_passwords': 0,
        'accepted_passwords': 0,
        'invalid_users': 0,
        'targeted_usernames': defaultdict(int),
        'attempt_times': [],
        'ports': set(),
        'reverse_dns_failures': 0,
    })

    username_enum = defaultdict(lambda: defaultdict(int)) # username -> IP -> count
    reverse_dns_anomalies = defaultdict(int)

    # Functions to parse timestamp
    def parse_timestamp(month_str, day_str, time_str):
        month = month_map.get(month_str, 1)
        day = int(day_str)
        # Compose datetime string YYYY-MM-DD HH:MM:SS
        try:
            dt = datetime.strptime(f"{current_year}-{month:02d}-{day:02d} {time_str}", "%Y-%m-%d %H:%M:%S")
        except Exception:
            dt = None
        return dt

    with open(log_path, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # Check reverse DNS failure line first
            m_rdns = re_reverse_dns_fail.search(line)
            if m_rdns:
                ip = m_rdns.group('ip')
                reverse_dns_anomalies[ip] += 1
                ip_profiles[ip]['reverse_dns_failures'] += 1
                continue

            # Failed password
            m_fail = re_failed_pass.match(line)
            if m_fail:
                ip = m_fail.group('ip')
                user = m_fail.group('user')
                port = m_fail.group('port')
                dt = parse_timestamp(m_fail.group('month'), m_fail.group('day'), m_fail.group('time'))

                ip_profiles[ip]['attempts'] += 1
                ip_profiles[ip]['failed_passwords'] += 1
                ip_profiles[ip]['targeted_usernames'][user] += 1
                if dt:
                    ip_profiles[ip]['attempt_times'].append(dt.timestamp())
                ip_profiles[ip]['ports'].add(int(port))
                username_enum[user][ip] += 1
                continue

            # Accepted password
            m_acc = re_accepted_pass.match(line)
            if m_acc:
                ip = m_acc.group('ip')
                user = m_acc.group('user')
                port = m_acc.group('port')
                dt = parse_timestamp(m_acc.group('month'), m_acc.group('day'), m_acc.group('time'))

                ip_profiles[ip]['attempts'] += 1
                ip_profiles[ip]['accepted_passwords'] += 1
                ip_profiles[ip]['targeted_usernames'][user] += 1
                if dt:
                    ip_profiles[ip]['attempt_times'].append(dt.timestamp())
                ip_profiles[ip]['ports'].add(int(port))
                username_enum[user][ip] += 1
                continue

            # Invalid user (usually from invalid user line, counts as fail)
            m_inv = re_invalid_user.match(line)
            if m_inv:
                ip = m_inv.group('ip')
                user = m_inv.group('user')
                port = m_inv.group('port')
                dt = parse_timestamp(m_inv.group('month'), m_inv.group('day'), m_inv.group('time'))

                ip_profiles[ip]['attempts'] += 1
                ip_profiles[ip]['invalid_users'] += 1
                ip_profiles[ip]['targeted_usernames'][user] += 1
                if dt:
                    ip_profiles[ip]['attempt_times'].append(dt.timestamp())
                ip_profiles[ip]['ports'].add(int(port))
                username_enum[user][ip] += 1
                continue

    # Analyze username enumeration patterns: flag usernames targeted by multiple IPs or many attempts
    username_enumeration_report = {}
    for user, ips in username_enum.items():
        total_attempts = sum(ips.values())
        distinct_ips = len(ips)
        if distinct_ips > 3 or total_attempts > 10:  # arbitrary heuristic thresholds
            username_enumeration_report[user] = {
                'total_attempts': total_attempts,
                'distinct_ips': distinct_ips,
                'ip_counts': ips
            }

    # Calculate time windows per IP (earliest to latest attempt)
    for ip, prof in ip_profiles.items():
        times = prof['attempt_times']
        if times:
            prof['time_window'] = {
                "start": min(times),
                "end": max(times),
                "duration_seconds": max(times)-min(times)
            }
        else:
            prof['time_window'] = None
        # Convert ports set to sorted list
        prof['ports'] = sorted(prof['ports'])
        # Adjust targeted_usernames dict to normal dict
        prof['targeted_usernames'] = dict(prof['targeted_usernames'])

    attack_stats = {
        'ip_profiles': dict(ip_profiles),
        'username_enumeration_patterns': username_enumeration_report,
        'reverse_dns_anomalies': dict(reverse_dns_anomalies)
    }

    # Write JSON output
    with open(out_path, 'w', encoding='utf-8') as outf:
        json.dump(attack_stats, outf, indent=2, sort_keys=True)

    return None