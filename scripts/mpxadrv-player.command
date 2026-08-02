#!/bin/zsh

setopt NO_CASE_GLOB NULL_GLOB

script_path=${0:A}
repo_dir=${script_path:h:h}
music_dir=${1:-$PWD}
player_override=""
soundfont_override=""

if [[ -n "${MPXADRV_BIN:-}" ]]; then
  player_override=${MPXADRV_BIN:A}
fi
if [[ -n "${MPXADRV_SOUNDFONT:-}" ]]; then
  soundfont_override=${MPXADRV_SOUNDFONT:A}
fi

if [[ ! -d "$music_dir" ]]; then
  print -u2 -- "フォルダーが見つかりません: $music_dir"
  exit 1
fi
cd -- "$music_dir" || exit 1

if [[ -n "$player_override" ]]; then
  if [[ ! -x "$player_override" ]]; then
    print -u2 -- "mpxadrvを実行できません: $player_override"
    exit 1
  fi
  player=$player_override
elif [[ -x "$repo_dir/build/mpxadrv" ]]; then
  player="$repo_dir/build/mpxadrv"
elif (( $+commands[mpxadrv] )); then
  player=$commands[mpxadrv]
else
  print -u2 -- "mpxadrvが見つかりません。先に cmake --build build を実行してください。"
  exit 1
fi

soundfont=""
if [[ -n "$soundfont_override" ]]; then
  if [[ ! -f "$soundfont_override" ]]; then
    print -u2 -- "SoundFontが見つかりません: $soundfont_override"
    exit 1
  fi
  soundfont=$soundfont_override
elif [[ -f "$repo_dir/SoundFonts/Roland_SC-55.sf2" ]]; then
  soundfont="$repo_dir/SoundFonts/Roland_SC-55.sf2"
fi

while true; do
  files=( *.mdr *.mdx )
  files=( ${(on)files} )

  print
  print -- "mpxadrv 選曲メニュー"
  print -- "フォルダー: $PWD"
  if [[ -n "$soundfont" ]]; then
    print -- "SoundFont: ${soundfont:t}"
  else
    print -- "SoundFont: macOS標準音源"
  fi
  print

  if (( ${#files} == 0 )); then
    print -- ".MDR／.MDXファイルがありません。"
  else
    integer index=1
    for file in "${files[@]}"; do
      printf '%3d) %s\n' $index "$file"
      (( ++index ))
    done
  fi

  print
  read "choice?番号を入力（r: 再読込 / q: 終了）> " || exit 0
  case ${(L)choice} in
    q|quit|exit)
      exit 0
      ;;
    r|reload)
      continue
      ;;
  esac

  if [[ "$choice" != <-> ]] || (( choice < 1 || choice > ${#files} )); then
    print -u2 -- "選択が正しくありません: $choice"
    continue
  fi

  selected=${files[choice]}
  play_command=( "$player" play "$selected" )
  if [[ "${(L)selected}" == *.mdr && -n "$soundfont" ]]; then
    play_command+=( --soundfont "$soundfont" )
  fi

  print
  print -- "再生: $selected"
  "${play_command[@]}"
  exit_code=$?
  if (( exit_code != 0 && exit_code != 130 )); then
    print -u2 -- "再生に失敗しました（終了コード: $exit_code）"
  fi
done
