#!/bin/sh
# Upload web/dist to https://ruzzoli.de/roguelikes/prime/
set -e
cd "$(dirname "$0")"
git fetch -q && [ -z "$(git status --porcelain)" ] && [ "$(git rev-parse @)" = "$(git rev-parse @{u})" ] || { echo "commit + push first" >&2; exit 1; }
ssh ruzzoli.de 'sudo mkdir -p /var/www/ruzzoli.de/roguelikes/prime && sudo chown -R felix:www-data /var/www/ruzzoli.de/roguelikes'
rsync -rtz --delete dist/ ruzzoli.de:/var/www/ruzzoli.de/roguelikes/prime/
