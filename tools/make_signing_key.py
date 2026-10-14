#!/usr/bin/env python3
"""Creates firmware/keys/igc_signing.key: 32 random bytes as hex for the IGC G record.

The key is secret and stays out of git (.gitignore). Keep a backup: files signed
with it can only be verified with the same key (tools/vbv_igc.py --verify).
"""
import os
import secrets
import sys

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "keys", "igc_signing.key")
path = os.path.normpath(path)
if os.path.exists(path):
    sys.exit(f"{path} exists - not overwritten")
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "w", encoding="ascii") as f:
    f.write(secrets.token_hex(32) + "\n")
print(f"created {path}")
