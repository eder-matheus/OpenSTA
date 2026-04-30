#!/usr/bin/env bash
# Liberty Parse Cache (LPC) benchmark.
#
# For each input .lib, runs phase 1 (write_lpc -> .lpc + write_liberty
# -> from_source.lib + Tcl-introspection dump -> from_source.dump
# [+ report_checks -> from_source.rpt]) and phase 2 (read_lpc +
# write_liberty -> from_cache.lib + Tcl dump -> from_cache.dump
# [+ report_checks -> from_cache.rpt]) in two fresh STA processes --
# so the cache load isn't measuring a warm in-process state -- then
# runs up to three equivalence diffs:
#
#   emit diff:   from_source.lib  vs from_cache.lib    (write_liberty bytes)
#   dump diff:   from_source.dump vs from_cache.dump   (Tcl introspection)
#   report diff: from_source.rpt  vs from_cache.rpt    (report_checks output;
#                                                       only when a design
#                                                       is mapped to the lib)
#
# Identical across all enabled checks means LPC preserves the
# write_liberty subset, the Tcl-visible subset (port flag bits,
# internal_power, leakage_power, sequentials, statetable, ...), AND
# the actual delay-calc results on a real netlist. FAIL is tagged
# with a comma-separated list of the failing checks: FAIL(emit),
# FAIL(dump), FAIL(report), or combinations.
#
# Usage:
#   test/lpc_benchmark/run.sh                    # sweep every relevant .lib in the repo
#   test/lpc_benchmark/run.sh LIB [LIB...]       # explicit list of .lib paths
#
# Optional environment:
#   STA_EXE             path to the sta binary; defaults to ./build/sta
#   STA_NO_KEEP         if non-empty, remove each lib's .lpc file (and
#                       diff on PASS) after the run -- the two .lib
#                       re-emissions are always preserved for
#                       visualization.
#   STA_KEEP_GOING      if non-empty, continue past the first failure.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BENCH_DIR="${REPO_ROOT}/test/lpc_benchmark"
OUT_DIR="${BENCH_DIR}/out"
STA_EXE="${STA_EXE:-${REPO_ROOT}/build/sta}"

if [ ! -x "$STA_EXE" ]; then
  echo "ERROR: sta executable not found or not executable: $STA_EXE" >&2
  echo "Set STA_EXE or build first: cmake --build build -j 8" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

# === Lib discovery ===================================================
# Sweep mode: collect every .lib under test/ and liberty/test/, sorted
# by size so small fixtures fail fast and large PDKs are last.
discover_libs () {
  # Exclude *_results dirs (write_liberty outputs from other tests)
  # and any */out/* (generated artifacts from this or sibling
  # benchmark suites left over in the working tree).
  find "${REPO_ROOT}/test" "${REPO_ROOT}/liberty/test" \
       -name "*.lib" -type f \
       ! -path "*/results/*" \
       ! -path "*/out/*" \
       -printf '%s\t%p\n' 2>/dev/null \
    | sort -n | cut -f2-
}

# === Per-lib runner ==================================================
run_phase () {
  local phase_tcl="$1" lib_path="$2" lpc_path="$3" golden="$4" recovered="$5"
  shift 5
  local -a phase_env=(
    STA_LIB_PATH="$lib_path"
    STA_LPC_PATH="$lpc_path"
    STA_GOLDEN_LIB="$golden"
    STA_RECOVERED_LIB="$recovered"
    "$@"
  )
  env "${phase_env[@]}" "$STA_EXE" -no_init -no_splash -exit "$phase_tcl" 2>&1
}

parse_phase_fields () {
  local -n out=$1
  local raw="$2"
  local key val
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    key="${line%%:*}"
    val="${line#*:}"
    val="${val#"${val%%[![:space:]]*}"}"
    out[$key]=$val
  done <<< "$raw"
}

ms () { awk -v u="${1:-0}" 'BEGIN{printf "%.1f", u/1000.0}'; }

human_bytes () {
  awk -v b="${1:-0}" 'BEGIN{
    if (b+0 == 0)         { print "?"; exit }
    if (b < 1024)         { printf "%dB",   b; exit }
    if (b < 1024*1024)    { printf "%.1fK", b/1024; exit }
    printf "%.1fM", b/1024/1024;
  }'
}

# Per-lib design lookup for phase 4. Returns 3 lines: verilog, sdc,
# top -- or no output if no design is mapped.
report_design_for () {
  case "$1" in
    */test/sky130hd/*.lib)
      printf '%s\n%s\n%s\n' \
        "${REPO_ROOT}/test/sky130hd/designs/jpeg.v" \
        "${REPO_ROOT}/test/sky130hd/designs/jpeg.sdc" \
        "jpeg_encoder"
      ;;
    */test/nangate45/Nangate45_lvt.lib)
      # _lvt cells use *_L suffix and don't match gcd's unsuffixed
      # instances -- linking would produce only black boxes.
      ;;
    */test/nangate45/Nangate45_*.lib)
      printf '%s\n%s\n%s\n' \
        "${REPO_ROOT}/test/nangate45/designs/gcd.v" \
        "${REPO_ROOT}/test/nangate45/designs/gcd.sdc" \
        "gcd"
      ;;
  esac
}

run_one_lib () {
  local lib_path="$1"
  local rel="${lib_path#${REPO_ROOT}/}"
  local sub="${rel%.lib}"
  local out_subdir="${OUT_DIR}/${sub}"
  mkdir -p "$out_subdir"

  local lpc_path="${out_subdir}/lib.lpc"
  local golden="${out_subdir}/from_source.lib"
  local recovered="${out_subdir}/from_cache.lib"
  local diff_path="${out_subdir}/lib.diff"
  local source_dump="${out_subdir}/from_source.dump"
  local cache_dump="${out_subdir}/from_cache.dump"
  local dump_diff="${out_subdir}/dump.diff"
  local source_report="${out_subdir}/from_source.rpt"
  local cache_report="${out_subdir}/from_cache.rpt"
  local report_diff="${out_subdir}/report.diff"

  rm -f "$lpc_path" "$golden" "$recovered" "$diff_path" \
        "$source_dump" "$cache_dump" "$dump_diff" \
        "$source_report" "$cache_report" "$report_diff"

  local report_v="" report_s="" report_t=""
  { read -r report_v; read -r report_s; read -r report_t; } < <(report_design_for "$lib_path") || true

  local -a env1=(STA_DUMP_OUT="$source_dump")
  local -a env2=(STA_DUMP_OUT="$cache_dump")
  if [ -n "$report_v" ]; then
    env1+=(STA_REPORT_VERILOG="$report_v"
           STA_REPORT_SDC="$report_s"
           STA_REPORT_TOP="$report_t"
           STA_REPORT_OUT="$source_report")
    env2+=(STA_REPORT_VERILOG="$report_v"
           STA_REPORT_SDC="$report_s"
           STA_REPORT_TOP="$report_t"
           STA_REPORT_OUT="$cache_report")
  fi

  local p1 p2 phase_err=""
  if ! p1=$(run_phase "${BENCH_DIR}/phase1_write.tcl" \
                      "$lib_path" "$lpc_path" "$golden" "$recovered" \
                      "${env1[@]}"); then
    phase_err="phase1"
  elif ! p2=$(run_phase "${BENCH_DIR}/phase2_read.tcl" \
                        "$lib_path" "$lpc_path" "$golden" "$recovered" \
                        "${env2[@]}"); then
    phase_err="phase2"
  fi

  if [ -n "$phase_err" ]; then
    LIB_RESULTS+=("$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
      "$rel" "$(wc -c < "$lib_path" 2>/dev/null || echo 0)" "0" \
      "0" "0" "n/a" "?" "?" "?" \
      "ERROR($phase_err)" "0" "0" "0")")
    LIB_VERDICTS+=("ERROR")
    return 3
  fi

  local -A f1 f2
  parse_phase_fields f1 "$p1"
  parse_phase_fields f2 "$p2"
  local lib_bytes="${f1[lib_bytes]:-}"
  local parse_us="${f1[parse_us]:-}"
  local lpc_bytes="${f1[lpc_bytes]:-}"
  local src_cells="${f1[cell_count]:-}"
  local load_us="${f2[load_us]:-}"
  local recovered_bytes="${f2[recovered_bytes]:-}"
  local dst_cells="${f2[cell_count]:-}"
  local dst_arcs="${f2[arc_sets]:-}"
  local dst_ports="${f2[ports]:-}"

  local speedup="n/a"
  if [ -n "${parse_us:-}" ] && [ -n "${load_us:-}" ] && [ "${load_us:-0}" -gt 0 ]; then
    speedup=$(awk -v p="$parse_us" -v l="$load_us" 'BEGIN{printf "%.1f", p/l}')
  fi

  # Three equivalence checks. Use `|| true` for diff (exit-1 on
  # files-differ is normal). Inspect file size to decide.
  local verdict diff_lines=0 dump_diff_lines=0 report_diff_lines=0
  local emit_pass=0 dump_pass=0 report_pass=1
  if [ ! -s "$golden" ] || [ ! -s "$recovered" ]; then
    verdict="SKIPPED"
  else
    diff "$golden" "$recovered" > "$diff_path" 2>/dev/null || true
    if [ ! -s "$diff_path" ]; then
      emit_pass=1
      rm -f "$diff_path"
    else
      diff_lines=$(wc -l < "$diff_path" 2>/dev/null || echo 0)
    fi

    if [ -s "$source_dump" ] && [ -s "$cache_dump" ]; then
      diff "$source_dump" "$cache_dump" > "$dump_diff" 2>/dev/null || true
      if [ ! -s "$dump_diff" ]; then
        dump_pass=1
        rm -f "$dump_diff"
      else
        dump_diff_lines=$(wc -l < "$dump_diff" 2>/dev/null || echo 0)
      fi
    else
      dump_pass=1
    fi

    if [ -s "$source_report" ] && [ -s "$cache_report" ]; then
      report_pass=0
      diff "$source_report" "$cache_report" > "$report_diff" 2>/dev/null || true
      if [ ! -s "$report_diff" ]; then
        report_pass=1
        rm -f "$report_diff"
      else
        report_diff_lines=$(wc -l < "$report_diff" 2>/dev/null || echo 0)
      fi
    fi

    if [ "$emit_pass" -eq 1 ] && [ "$dump_pass" -eq 1 ] && [ "$report_pass" -eq 1 ]; then
      verdict="PASS"
    else
      local -a tags=()
      [ "$emit_pass"   -eq 0 ] && tags+=("emit")
      [ "$dump_pass"   -eq 0 ] && tags+=("dump")
      [ "$report_pass" -eq 0 ] && tags+=("report")
      verdict="FAIL($(IFS=,; echo "${tags[*]}"))"
    fi
  fi

  if [ -n "${STA_NO_KEEP:-}" ]; then
    rm -f "$lpc_path"
    if [[ "$verdict" != FAIL* ]]; then
      rm -f "$diff_path" "$dump_diff" "$report_diff" 2>/dev/null || true
    fi
  fi

  LIB_RESULTS+=("$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
    "$rel" "${lib_bytes:-0}" "${lpc_bytes:-0}" \
    "${parse_us:-0}" "${load_us:-0}" "${speedup}" \
    "${dst_cells:-?}" "${dst_arcs:-?}" "${dst_ports:-?}" \
    "${verdict}" "${diff_lines}" "${dump_diff_lines}" "${report_diff_lines}")")
  LIB_VERDICTS+=("$verdict")

  case "$verdict" in
    PASS)    return 0 ;;
    FAIL*)   return 1 ;;
    SKIPPED) return 2 ;;
    *)       return 3 ;;
  esac
}

# === Main ============================================================
declare -a LIBS
if [ $# -eq 0 ]; then
  mapfile -t LIBS < <(discover_libs)
  if [ ${#LIBS[@]} -eq 0 ]; then
    echo "ERROR: no .lib files found under test/ or liberty/test/" >&2
    exit 1
  fi
else
  for arg in "$@"; do
    if [ ! -f "$arg" ]; then
      echo "ERROR: liberty file not found: $arg" >&2
      exit 1
    fi
    LIBS+=("$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")")
  done
fi

echo "=== Liberty Parse Cache (LPC) benchmark ==="
echo "sta:        $STA_EXE"
echo "out_dir:    $OUT_DIR"
echo "libs:       ${#LIBS[@]}"
echo

declare -a LIB_RESULTS=()
declare -a LIB_VERDICTS=()
overall_status=0
i=0
for lib in "${LIBS[@]}"; do
  i=$((i+1))
  rel="${lib#${REPO_ROOT}/}"
  printf "  [%2d/%2d] %s ... " "$i" "${#LIBS[@]}" "$rel"
  set +e
  run_one_lib "$lib"
  rc=$?
  set -e
  case "$rc" in
    0) echo "PASS" ;;
    1) echo "FAIL"
       overall_status=1
       ;;
    2) echo "SKIPPED" ;;
    3) echo "ERROR"
       overall_status=1
       ;;
  esac
  if [ "$rc" -eq 1 ] || [ "$rc" -eq 3 ]; then
    if [ -z "${STA_KEEP_GOING:-}" ]; then
      echo
      echo "Stopping after first failure (set STA_KEEP_GOING=1 to continue sweep)."
      break
    fi
  fi
done

# === Summary table ===================================================
echo
echo "=== summary ==="
header=$(printf '%-58s  %8s  %8s  %8s  %8s  %8s  %5s  %5s  %5s  %s\n' \
  "lib" "src" "lpc" "parse" "load" "speedup" "cells" "arcs" "ports" "verdict")
echo "$header"
echo "$header" | sed 's/[^ ]/-/g'
for row in "${LIB_RESULTS[@]}"; do
  IFS=$'\t' read -r rel lib_b lpc_b parse_us load_us speedup cells arcs ports verdict diff_lines dump_diff_lines report_diff_lines <<< "$row"
  speedup_disp="${speedup}x"
  [ "$speedup" = "n/a" ] && speedup_disp="n/a"
  detail=""
  if [[ "$verdict" == FAIL* ]]; then
    detail=" (emit=$diff_lines, dump=$dump_diff_lines, report=$report_diff_lines diff lines)"
  fi
  printf '%-58s  %8s  %8s  %8s  %8s  %8s  %5s  %5s  %5s  %s%s\n' \
    "$rel" \
    "$(human_bytes "$lib_b")" \
    "$(human_bytes "$lpc_b")" \
    "$(ms "$parse_us")ms" \
    "$(ms "$load_us")ms" \
    "$speedup_disp" \
    "$cells" "$arcs" "$ports" \
    "$verdict" "$detail"
done

# === Overall verdict =================================================
echo
total=${#LIB_VERDICTS[@]}
pass=0; fail=0; err=0; skip=0
for v in "${LIB_VERDICTS[@]}"; do
  case "$v" in
    PASS)    pass=$((pass+1)) ;;
    FAIL*)   fail=$((fail+1)) ;;
    ERROR)   err=$((err+1));  overall_status=1 ;;
    SKIPPED) skip=$((skip+1)) ;;
  esac
done
echo "=== ${pass}/${total} PASS, ${fail} FAIL, ${err} ERROR, ${skip} SKIPPED ==="

if [ ${#LIBS[@]} -eq 1 ] || [ "$fail" -gt 0 ] || [ "$err" -gt 0 ]; then
  echo
  echo "=== outputs (open these to visualize) ==="
  for row in "${LIB_RESULTS[@]}"; do
    IFS=$'\t' read -r rel _ _ _ _ _ _ _ _ verdict _ _ _ <<< "$row"
    case "$verdict" in
      PASS) [ ${#LIBS[@]} -eq 1 ] || continue ;;
      SKIPPED) continue ;;
    esac
    sub="${OUT_DIR}/${rel%.lib}"
    echo "  $rel  [$verdict]"
    [ -s "${sub}/from_source.lib"  ] && echo "    parsed-lib  : ${sub}/from_source.lib"
    [ -s "${sub}/from_cache.lib"   ] && echo "    cache-load  : ${sub}/from_cache.lib"
    [ -s "${sub}/lib.diff"         ] && echo "    emit diff   : ${sub}/lib.diff"
    [ -s "${sub}/from_source.dump" ] && echo "    parsed-dump : ${sub}/from_source.dump"
    [ -s "${sub}/from_cache.dump"  ] && echo "    cache-dump  : ${sub}/from_cache.dump"
    [ -s "${sub}/dump.diff"        ] && echo "    dump diff   : ${sub}/dump.diff"
    [ -s "${sub}/from_source.rpt"  ] && echo "    parsed-rpt  : ${sub}/from_source.rpt"
    [ -s "${sub}/from_cache.rpt"   ] && echo "    cache-rpt   : ${sub}/from_cache.rpt"
    [ -s "${sub}/report.diff"      ] && echo "    report diff : ${sub}/report.diff"
  done
fi

exit "$overall_status"
