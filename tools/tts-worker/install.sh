#!/bin/sh
# Sets up the TTS worker on a Debian 13 host or container (as root, from /opt/tts/worker, where
# deploy.sh copies this folder): user tts, Python venv, Chatterbox at a pinned commit, the model
# files at pinned revisions (about 4 GB: only what the multilingual loader and the meanflow decoder
# need, not the 14 GB repository), systemd units. Safe to run again.
set -eu

CHATTERBOX_COMMIT=5de7a54aa4e5e2baadb0182dde554908b48b85c2
HF_CHATTERBOX=5bb1f6ee58e50c3b8d408bc82a6d3740c2db6e18
HF_TURBO=749d1c1a46eb10492095d68fbcf55691ccf137cd
HERE=$(cd "$(dirname "$0")" && pwd)

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq git python3-venv ca-certificates curl >/dev/null

id tts >/dev/null 2>&1 || useradd --system --home-dir /var/lib/tts --no-create-home --shell /usr/sbin/nologin tts
mkdir -p /opt/tts/src /opt/tts/models/chatterbox /opt/tts/models/chatterbox-turbo /etc/tts \
  /var/lib/tts/voices /var/lib/tts/private /var/lib/tts/packs /var/lib/tts/cache
chown -R tts:tts /var/lib/tts
chmod 750 /var/lib/tts
chmod 700 /var/lib/tts/private  # other people's voices

# Python and Chatterbox
[ -x /opt/tts/venv/bin/python ] || python3 -m venv /opt/tts/venv
/opt/tts/venv/bin/pip install -q --upgrade pip
/opt/tts/venv/bin/pip install -q -r "$HERE/requirements.txt"
[ -d /opt/tts/src/chatterbox ] || git clone -q https://github.com/resemble-ai/chatterbox.git /opt/tts/src/chatterbox
git -C /opt/tts/src/chatterbox fetch -q origin
git -C /opt/tts/src/chatterbox checkout -q "$CHATTERBOX_COMMIT"
/opt/tts/venv/bin/pip install -q --no-deps -e /opt/tts/src/chatterbox

# Models (MIT, Resemble AI), each file only once
fetch() { # repo revision file folder
  [ -s "$4/$3" ] && return 0
  curl -sSfL --retry 3 -o "$4/$3.part" "https://huggingface.co/$1/resolve/$2/$3"
  mv "$4/$3.part" "$4/$3"
}
for f in ve.pt t3_mtl23ls_v3.safetensors s3gen.pt grapheme_mtl_merged_expanded_v1.json conds.pt Cangjie5_TC.json; do
  fetch ResembleAI/chatterbox "$HF_CHATTERBOX" "$f" /opt/tts/models/chatterbox
done
fetch ResembleAI/chatterbox-turbo "$HF_TURBO" s3gen_meanflow.safetensors /opt/tts/models/chatterbox-turbo
echo "$HF_CHATTERBOX" > /opt/tts/models/chatterbox/REVISION
echo "$HF_TURBO" > /opt/tts/models/chatterbox-turbo/REVISION

# Settings (the secret stays out of the unit files)
if [ ! -f /etc/tts/worker.env ]; then
  cp "$HERE/worker.env.example" /etc/tts/worker.env
  echo "edit /etc/tts/worker.env: WORKER_SECRET (the same as in the relay's .env)"
fi
chown root:tts /etc/tts/worker.env
chmod 640 /etc/tts/worker.env

cp "$HERE/systemd/tts-worker.service" "$HERE/systemd/tts-lab.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now tts-lab.service
if grep -qE '^WORKER_SECRET=.{16,}' /etc/tts/worker.env; then
  systemctl enable tts-worker.service
  systemctl restart tts-worker.service
else
  echo "tts-worker not started: no WORKER_SECRET in /etc/tts/worker.env"
fi
echo "done: lab on http://$(hostname -I | cut -d' ' -f1):7860, worker: journalctl -u tts-worker -f"
