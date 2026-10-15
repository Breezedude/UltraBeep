#!/usr/bin/env bash
# Copies the worker to the home server, with the current voice pack texts, and restarts its services:
#   tools/tts-worker/deploy.sh root@<host> [--install]
# --install runs install.sh there (first setup, or after requirements.txt changed).
# SSH_KEY picks a key file; without it ssh uses its agent and ~/.ssh/config.
set -euo pipefail
host=${1:?usage: deploy.sh root@<host> [--install]}
here=$(cd "$(dirname "$0")" && pwd)
ssh_opts=(-o BatchMode=yes)
[ -n "${SSH_KEY:-}" ] && ssh_opts+=(-i "$SSH_KEY" -o IdentitiesOnly=yes)

(cd "$here" && tar -cf - --exclude=__pycache__ ./*.py ./*.sh requirements.txt worker.env.example systemd tests) |
  ssh "${ssh_opts[@]}" "$host" "mkdir -p /opt/tts/worker && tar -C /opt/tts/worker --no-same-owner -xf -"
node "$here/export_catalogue.js" | ssh "${ssh_opts[@]}" "$host" "cat > /opt/tts/catalogue.json"
if [ "${2:-}" = "--install" ]; then
  ssh "${ssh_opts[@]}" "$host" "sh /opt/tts/worker/install.sh"
else
  ssh "${ssh_opts[@]}" "$host" "for s in tts-worker tts-lab; do systemctl is-enabled -q \$s && systemctl restart \$s; done; systemctl is-active tts-worker tts-lab || true"
fi
