#!/bin/zsh

setopt NO_CASE_GLOB NULL_GLOB

script_path=${0:A}
repo_dir=${script_path:h:h}
catalog_source="${MPXADRV_CATALOG:-}"
music_dir=""
player_override=""
soundfont_override=""
# Runtime output mode: "" = software, otherwise CoreMIDI selector.
destination_override="${MPXADRV_DESTINATION:-}"
catalog_mode=0

while (( $# > 0 )); do
  case $1 in
    --catalog)
      shift
      if (( $# == 0 )); then
        print -u2 -- "--catalog には catalog.json か URL を指定してください。"
        exit 1
      fi
      catalog_source=$1
      catalog_mode=1
      shift
      ;;
    -h|--help)
      print -- "使い方: mpxadrv-player [フォルダー]"
      print -- "       mpxadrv-player --catalog <catalog.json|url>"
      print -- "環境変数: MPXADRV_CATALOG, MPXADRV_BIN, MPXADRV_SOUNDFONT, MPXADRV_DESTINATION"
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      print -u2 -- "不明なオプション: $1"
      exit 1
      ;;
    *)
      if [[ -z "$music_dir" ]]; then
        music_dir=$1
      else
        print -u2 -- "引数が多すぎます: $1"
        exit 1
      fi
      shift
      ;;
  esac
done

if (( catalog_mode == 0 )) && [[ -z "$catalog_source" ]]; then
  music_dir=${music_dir:-$PWD}
  if [[ ! -d "$music_dir" ]]; then
    print -u2 -- "フォルダーが見つかりません: $music_dir"
    exit 1
  fi
  cd -- "$music_dir" || exit 1
elif [[ -z "$catalog_source" ]]; then
  print -u2 -- "カタログが指定されていません。"
  exit 1
fi

resolve_path() {
  local value=$1
  if [[ -z "$value" ]]; then
    print -n -- ""
    return
  fi
  # Expand leading ~ manually; :A alone does not treat ~/foo as home.
  if [[ "$value" == '~' || "$value" == '~/'* ]]; then
    value="${HOME}${value#\~}"
  fi
  if [[ "$value" == /* ]]; then
    print -n -- "$value"
    return
  fi
  if [[ -e "$repo_dir/$value" ]]; then
    print -n -- "${repo_dir:A}/$value"
    return
  fi
  print -n -- "${value:A}"
}

if [[ -n "${MPXADRV_BIN:-}" ]]; then
  player_override=$(resolve_path "$MPXADRV_BIN")
fi
if [[ -n "${MPXADRV_SOUNDFONT:-}" ]]; then
  soundfont_override=$(resolve_path "$MPXADRV_SOUNDFONT")
fi

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

if [[ -n "$destination_override" && -n "$soundfont_override" ]]; then
  print -u2 -- "MPXADRV_DESTINATION と MPXADRV_SOUNDFONT は同時に指定できません。"
  exit 1
fi

repo_default_soundfont="$repo_dir/SoundFonts/Roland_SC-55.sf2"
default_soundfont=""
soundfont_warning=""
if [[ -n "$soundfont_override" ]]; then
  if [[ -f "$soundfont_override" ]]; then
    default_soundfont=$soundfont_override
  elif [[ -f "$repo_default_soundfont" ]]; then
    soundfont_warning="MPXADRV_SOUNDFONT 不在 → リポジトリの SoundFont を使用"
    default_soundfont=$repo_default_soundfont
  else
    print -u2 -- "SoundFontが見つかりません: $soundfont_override"
    print -u2 -- "例: export MPXADRV_SOUNDFONT=\"$repo_default_soundfont\""
    exit 1
  fi
elif [[ -f "$repo_default_soundfont" ]]; then
  default_soundfont=$repo_default_soundfont
fi
# Active software bank (used when destination is empty).
soundfont=$default_soundfont

all_files=()
catalog_mdr=()
catalog_pdx=()
visible=()
visible_indices=()
filter=""
page=1
status_message="${soundfont_warning}"
play_command=()
selected_label=""

reload_files() {
  all_files=()
  catalog_mdr=()
  catalog_pdx=()
  if (( catalog_mode )); then
    local index id title mdr_url pdx_url rest
    while IFS=$'\t' read -r index id title mdr_url pdx_url rest; do
      # Ignore banners / blank lines; only numbered TSV rows count.
      [[ "$index" == <-> ]] || continue
      # Titles may historically contain stray TABs; recover the MDR URL field.
      if [[ "$mdr_url" != http://* && "$mdr_url" != https://* ]]; then
        local candidate
        for candidate in "$mdr_url" "$pdx_url" ${(s:\t:)rest}; do
          if [[ "$candidate" == http://* || "$candidate" == https://* ]]; then
            if [[ "$candidate" == *.mdr || "$candidate" == *.MDR ]]; then
              mdr_url=$candidate
            elif [[ "$candidate" == *.pdx || "$candidate" == *.PDX ]]; then
              pdx_url=$candidate
            elif [[ -z "$mdr_url" || "$mdr_url" != http* ]]; then
              mdr_url=$candidate
            fi
          fi
        done
      fi
      [[ "$mdr_url" == http://* || "$mdr_url" == https://* ]] || continue
      all_files+=( "${title:-$id}" )
      catalog_mdr+=( "$mdr_url" )
      catalog_pdx+=( "$pdx_url" )
    done < <("$player" catalog "$catalog_source" --tsv 2>/dev/null)
  else
    all_files=( *.mdr *.mdx )
    all_files=( ${(on)all_files} )
  fi
  apply_filter
}

apply_filter() {
  visible=()
  visible_indices=()
  if [[ -z "$filter" ]]; then
    local i
    for (( i = 1; i <= ${#all_files}; ++i )); do
      visible+=( "${all_files[i]}" )
      visible_indices+=( $i )
    done
  else
    local file lower
    local needle=${(L)filter}
    local i
    for (( i = 1; i <= ${#all_files}; ++i )); do
      file=${all_files[i]}
      lower=${(L)file}
      if [[ "$lower" == *"$needle"* ]]; then
        visible+=( "$file" )
        visible_indices+=( $i )
      fi
    done
  fi
  if (( page < 1 )); then
    page=1
  fi
}

page_size() {
  local lines=${LINES:-24}
  local size=$(( lines - 11 ))
  if (( size < 8 )); then
    size=8
  fi
  if (( size > 40 )); then
    size=40
  fi
  print -n -- $size
}

page_count() {
  local size=$1
  local total=${#visible}
  if (( total == 0 )); then
    print -n -- 1
    return
  fi
  print -n -- $(( (total + size - 1) / size ))
}

output_label() {
  if [[ -n "$destination_override" ]]; then
    print -n -- "外部MIDI: $destination_override"
  elif [[ -n "$soundfont" ]]; then
    print -n -- "内蔵ソフト: ${soundfont:t}"
  else
    print -n -- "内蔵ソフト: macOS標準音源"
  fi
}

draw_menu() {
  local size=$(page_size)
  local pages=$(page_count $size)
  if (( page > pages )); then
    page=$pages
  fi
  local start=$(( (page - 1) * size + 1 ))
  local end=$(( page * size ))
  if (( end > ${#visible} )); then
    end=${#visible}
  fi

  clear 2>/dev/null || true
  print -- "mpxadrv 選曲メニュー"
  if (( catalog_mode )); then
    print -- "カタログ: $catalog_source"
  else
    print -- "フォルダー: $PWD"
  fi
  print -- "出力: $(output_label)"
  if [[ -n "$filter" ]]; then
    print -- "絞り込み: /$filter  （該当: ${#visible} / 全${#all_files}）"
  else
    print -- "曲数: ${#all_files}"
  fi
  if (( ${#visible} > 0 )); then
    print -- "ページ: $page / $pages  （表示 ${start}-${end}）"
  else
    print -- "ページ: —"
  fi
  if [[ -n "$status_message" ]]; then
    print -- "--- $status_message"
  fi
  print

  if (( ${#visible} == 0 )); then
    if (( ${#all_files} == 0 )); then
      if (( catalog_mode )); then
        print -- "カタログに曲がありません。"
      else
        print -- ".MDR／.MDXファイルがありません。"
      fi
    else
      print -- "絞り込みに一致する曲がありません。"
    fi
  else
    local i
    for (( i = start; i <= end; ++i )); do
      printf '%3d) %s\n' $i "${visible[i]}"
    done
  fi

  print
  print -- "操作: 番号=再生  n/+=次  p/-=前  /文字=絞込  c=解除"
  print -- "      s=内蔵音源  d=外部MIDI  r=再読込  q=終了"
}

choose_software() {
  destination_override=""
  soundfont=$default_soundfont
  if [[ -n "$soundfont" ]]; then
    status_message="内蔵ソフトに切替: ${soundfont:t}"
  else
    status_message="内蔵ソフトに切替: macOS標準音源"
  fi
}

choose_destination() {
  local list
  list=$("$player" midi-list 2>/dev/null | sed -n 's/^\([0-9][0-9]*\): \(.*\)$/\1|\2/p')
  if [[ -z "$list" ]]; then
    status_message="CoreMIDI出力が見つかりません（midi-list が空）"
    return
  fi

  clear 2>/dev/null || true
  print -- "外部 MIDI 出力を選択"
  print -- "------------------"
  local line idx name
  while IFS='|' read -r idx name; do
    printf '%3d) %s\n' $idx "$name"
  done <<< "$list"
  print
  print -- "番号を入力（空Enterでキャンセル）"
  local pick
  read "pick? > " || return
  if [[ -z "$pick" ]]; then
    status_message="外部MIDIの選択をキャンセルしました"
    return
  fi
  if [[ "$pick" != <-> ]]; then
    status_message="番号が正しくありません: $pick"
    return
  fi
  local matched=""
  while IFS='|' read -r idx name; do
    if [[ "$idx" == "$pick" ]]; then
      matched=$name
      break
    fi
  done <<< "$list"
  if [[ -z "$matched" ]]; then
    status_message="その番号の出力はありません: $pick"
    return
  fi
  destination_override=$pick
  status_message="外部MIDIに切替: $pick ($matched)"
}

build_play_command() {
  local menu_index=$1
  local source_index=${visible_indices[menu_index]}
  selected_label=${visible[menu_index]}

  if (( catalog_mode )); then
    local mdr_url=${catalog_mdr[source_index]}
    local pdx_url=${catalog_pdx[source_index]}
    play_command=( "$player" play "$mdr_url" )
    if [[ -n "$pdx_url" ]]; then
      play_command+=( --pdx-url "$pdx_url" )
    fi
  else
    local selected=${all_files[source_index]}
    selected_label=$selected
    play_command=( "$player" play "$selected" )
    if [[ "${(L)selected}" == *.mdr ]]; then
      if [[ -n "$destination_override" ]]; then
        play_command+=( --destination "$destination_override" )
      elif [[ -n "$soundfont" ]]; then
        play_command+=( --soundfont "$soundfont" )
      fi
    fi
    return
  fi

  if [[ -n "$destination_override" ]]; then
    play_command+=( --destination "$destination_override" )
  elif [[ -n "$soundfont" ]]; then
    play_command+=( --soundfont "$soundfont" )
  fi
}

run_player() {
  # Keep the player in the foreground so Core Audio / FluidSynth can open the
  # output device. Ignore SIGINT only in this menu shell; restore default in a
  # subshell before exec so mpxadrv does not inherit SIG_IGN.
  trap '' INT
  ( trap - INT; exec "${play_command[@]}" )
  local code=$?
  trap - INT
  return $code
}

reload_files

while true; do
  draw_menu
  status_message=""
  read -r "choice? > " || {
    print -- "終了しました。"
    exit 0
  }
  # Strip CR from pastes / Terminal oddities.
  choice=${choice%$'\r'}
  # Ignore accidental cmake/Ninja progress lines pasted into the prompt.
  if [[ "$choice" == \[*\%*\]* || "$choice" == '[ '* ]]; then
    status_message="ビルドログの貼り付けを無視しました"
    continue
  fi

  case ${(L)choice} in
    q|quit|exit)
      print -- "終了しました。"
      exit 0
      ;;
    r|reload)
      reload_files
      status_message="一覧を再読込しました"
      continue
      ;;
    n|+)
      page=$(( page + 1 ))
      continue
      ;;
    p|-)
      if (( page > 1 )); then
        page=$(( page - 1 ))
      fi
      continue
      ;;
    c)
      filter=""
      page=1
      apply_filter
      status_message="絞り込みを解除しました"
      continue
      ;;
    s)
      choose_software
      continue
      ;;
    d)
      choose_destination
      continue
      ;;
  esac

  if [[ "$choice" == /* ]]; then
    filter=${choice#/}
    page=1
    apply_filter
    if [[ -z "$filter" ]]; then
      status_message="絞り込みを解除しました"
    else
      status_message="「$filter」で絞り込み（${#visible}曲）"
    fi
    continue
  fi

  if [[ "$choice" != <-> ]] || (( choice < 1 || choice > ${#visible} )); then
    status_message="選択が正しくありません: $choice"
    continue
  fi

  build_play_command $choice

  print
  print -- "再生: $selected_label"
  print -- "出力: $(output_label)"
  print -- "コマンド: ${play_command[*]}"
  print -- "（Ctrl-C で停止してメニューへ戻ります）"
  run_player
  exit_code=$?
  if (( exit_code != 0 && exit_code != 130 )); then
    status_message="再生に失敗しました（終了コード: $exit_code）"
  else
    status_message="停止: $selected_label / $(output_label)"
  fi
done
