# Cloudflare R2: host playable Reference/MDR songs

This guide puts **playable** MADRV songs from the local ignored
`Reference/MDR` folder onto Cloudflare R2 with public HTTPS URLs, then drives
`mpxadrv catalog` / `mpxadrv-player --catalog` from a generated `catalog.json`.

Copyrighted song data stays out of Git. Only the player and (optionally) a
public `catalog.json` URL are shared.

## What gets uploaded

From `Reference/MDR` (as of this writing):

| Category | Count | Action |
|----------|------:|--------|
| No PDX name (MIDI / FM without PCM bank) | 233 | Upload MDR |
| PDX name present and file found | 7 | Upload MDR + PDX |
| Declares PDX/TDX but bank missing | 11 | **Excluded** |

Staged payload is about **1.4 MB** — well inside R2's free 10 GB storage.

Excluded titles need a local PDX/TDX before they can be staged (see
`r2-stage/excluded.json` after a dry run / stage).

## 1. Create an R2 bucket

1. Open [Cloudflare Dashboard](https://dash.cloudflare.com/) → **R2 Object Storage**.
2. **Create bucket**, e.g. `mpxadrv-mdr`.
3. Open the bucket → **Settings** → **Public access**:
   - Enable **R2.dev subdomain**, or
   - Attach a **custom domain** (recommended for stable URLs).
4. Copy the public base URL, for example:
   - `https://pub-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.r2.dev`
   - `https://mdr.example.com`

Keep objects under a prefix such as `mdr/` so the catalog can use:

```text
https://pub-….r2.dev/mdr/BOMB.MDR
```

## 2. Stage local files

From the repository root (with your real public base URL):

```sh
python3 scripts/stage-r2-mdr.py --dry-run \
  --base-url 'https://pub-xxxxxxxx.r2.dev'

python3 scripts/stage-r2-mdr.py \
  --source Reference/MDR \
  --dest r2-stage \
  --base-url 'https://pub-xxxxxxxx.r2.dev' \
  --prefix mdr
```

This writes (gitignored):

```text
r2-stage/
  catalog.json      # titles + public mdr/pdx URLs
  excluded.json     # files skipped for missing banks
  SUMMARY.json
  songs/            # files to upload under the mdr/ prefix
```

## 3. Upload with Wrangler

Install and log in:

```sh
npm install -g wrangler
wrangler login
```

Upload the song objects under the `mdr/` prefix:

```sh
npx wrangler r2 object put mpxadrv-mdr/mdr/BOMB.MDR \
  --file=r2-stage/songs/BOMB.MDR
```

Bulk upload with a small loop:

```sh
bucket=mpxadrv-mdr
for path in r2-stage/songs/*; do
  name=${path##*/}
  npx wrangler r2 object put "${bucket}/mdr/${name}" --file="$path"
done
```

Or use [rclone](https://rclone.org/) after configuring an S3-compatible remote
for R2 (API token with Object Read & Write):

```sh
rclone copy r2-stage/songs/ r2:mpxadrv-mdr/mdr/ --progress
```

Optional: also publish the catalog next to the songs (public, titles only):

```sh
npx wrangler r2 object put mpxadrv-mdr/catalog.json \
  --file=r2-stage/catalog.json --content-type application/json
```

## 4. Play from the catalog

```sh
# After rebuild of 0.7.0+
cmake --build build

./build/mpxadrv catalog r2-stage/catalog.json
./build/mpxadrv catalog https://pub-xxxxxxxx.r2.dev/catalog.json

./scripts/mpxadrv-player.command --catalog r2-stage/catalog.json
# or:
MPXADRV_CATALOG=https://pub-xxxxxxxx.r2.dev/catalog.json \
  ./scripts/mpxadrv-player.command --catalog "$MPXADRV_CATALOG"
```

Single song without the menu:

```sh
./build/mpxadrv play 'https://pub-xxxxxxxx.r2.dev/mdr/BOMB.MDR' \
  --soundfont SoundFonts/Roland_SC-55.sf2
```

If a song needs a remote PDX:

```sh
./build/mpxadrv play 'https://…/mdr/NAMA47SC.MDR' \
  --pdx-url 'https://…/mdr/NAMA.PDX' \
  --soundfont SoundFonts/Roland_SC-55.sf2
```

(`catalog.json` already embeds `pdx` URLs; the player menu passes `--pdx-url`.)

## Notes

- Do **not** commit `Reference/` or `r2-stage/` — both are gitignored.
- Public R2 URLs mean anyone with the link can download the bytes. Prefer a
  hard-to-guess custom domain / path if you only want casual sharing, or switch
  later to signed URLs for stricter control.
- Rebuild `mpxadrv` after the catalog/URL merge (`0.7.0`) before testing remote
  playback; an older `build/mpxadrv` will not understand `catalog` / HTTPS input.
