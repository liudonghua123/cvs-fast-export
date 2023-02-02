#!/usr/bin/env python3
"""
Parse a nominally ISO 8601 date from stdin to a Unix integer timestamp.
"""
from datetime import datetime

import sys
date_str = sys.stdin.readline().rstrip()

print(int(datetime.strptime(date_str, "%Y-%m-%d %H:%M:%S %z").timestamp()))
