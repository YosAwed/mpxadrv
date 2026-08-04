#!/usr/bin/env bash
# Create the R2 bucket (if needed), enable r2.dev public access, upload staged
# songs + catalog, then rewrite catalog.json with the real public base URL.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

bucket="${R2_BUCKET:-mpxadrv-mdr}"
prefix="${R2_PREFIX:-mdr}"
stage="${R2_STAGE:-r2-stage}"
wrangler=(npx wrangler)

if [[ ! -d "$stage/songs" ]]; then
  echo "missing $stage/songs — run scripts/stage-r2-mdr.py first" >&2
  exit 1
fi

echo "==> Ensuring bucket exists: $bucket"
if ! "${wrangler[@]}" r2 bucket list 2>/dev/null | grep -q "$bucket"; then
  "${wrangler[@]}" r2 bucket create "$bucket"
fi

echo "==> Enabling public r2.dev access on $bucket"
"${wrangler[@]}" r2 bucket dev-url enable "$bucket" || true

echo "==> Resolving public base URL"
base_url="$("${wrangler[@]}" r2 bucket info "$bucket" 2>/dev/null | sed -n 's/.*https:\/\/pub-[a-z0-9.]*.r2.dev.*/&/p' | head -1 || true)"
if [[ -z "${base_url:-}" ]]; then
  # Fallback: wrangler prints the domain on enable / info.
  info="$("${wrangler[@]}" r2 bucket info "$bucket" 2>&1 || true)"
  echo "$info"
  base_url="$(printf '%s\n' "$info" | grep -Eo 'https://pub-[A-Za-z0-9.-]+\.r2\.dev' | head -1 || true)"
fi
if [[ -z "${base_url:-}" && -n "${R2_BASE_URL:-}" ]]; then
  base_url="$R2_BASE_URL"
fi
if [[ -z "${base_url:-}" ]]; then
  echo "Could not detect public r2.dev URL." >&2
  echo "Enable Public Development URL in the dashboard, then re-run with:" >&2
  echo "  R2_BASE_URL='https://pub-xxxx.r2.dev' $0" >&2
  exit 1
fi
echo "    base URL: $base_url"

echo "==> Restaging catalog with real public URLs"
python3 scripts/stage-r2-mdr.py \
  --source Reference/MDR \
  --dest "$stage" \
  --base-url "$base_url" \
  --prefix "$prefix"

echo "==> Uploading song objects"
count=0
for path in "$stage"/songs/*; do
  name=${path##*/}
  "${wrangler[@]}" r2 object put "${bucket}/${prefix}/${name}" --file="$path" --remote >/dev/null
  count=$((count + 1))
  if (( count % 25 == 0 )); then
    echo "    uploaded $count..."
  fi
done
echo "    uploaded $count objects under ${bucket}/${prefix}/"

echo "==> Uploading catalog.json"
"${wrangler[@]}" r2 object put "${bucket}/catalog.json" \
  --file="$stage/catalog.json" \
  --content-type application/json \
  --remote

cat <<EOF

Done.

Catalog (local):  $stage/catalog.json
Catalog (remote): ${base_url}/catalog.json

Try:
  cmake --build build
  ./build/mpxadrv catalog ${base_url}/catalog.json
  ./scripts/mpxadrv-player.command --catalog ${base_url}/catalog.json

EOF
