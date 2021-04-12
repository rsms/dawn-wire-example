#!/bin/bash
cd "$(dirname "$0")"
set -e

_err() { echo "$0: $1" >&2 ; exit 1 ; }

SRC_DIR=$PWD
OUT_DIR=
BUILD_MODE=debug
OPT_CONFIG=false
OPT_WATCH=false
OPT_RUN=
OPT_HELP=false

while [[ $# -gt 0 ]]; do case "$1" in
  -config)         OPT_CONFIG=true; shift ;;
  -opt)            BUILD_MODE=opt; shift ;;
  -w|-watch)       OPT_WATCH=true; shift ;;
  -run=*)          OPT_RUN="${1:5}"; shift ;;
  -outdir=*)       OUT_DIR="${1:8}"; shift ;;
  -h|-help|--help) OPT_HELP=true; shift ;;
  --)              shift; break ;;
  -*)              _err "Unknown option $1 (see $0 --help)" ;;
  *)               break ;;
esac; done
if $OPT_HELP; then
  cat <<- _EOF_ >&2
usage: $0 [options] [--] [<ninja-arg> ...]
options:
  -config        Reconfigure even if config seems up to date
  -opt           Build optimized build instead of debug build
  -w, -watch     Rebuild when source files change
  -run=<cmd>     Run <cmd> after building
  -outdir=<dir>  Use <dir> instead of out/{debug,opt}
  -h, -help      Show help and exit
_EOF_
  exit 1
fi

USER_PWD=$PWD

[ -n "$OUT_DIR" ] || OUT_DIR=out/$BUILD_MODE

if $OPT_CONFIG || [ ! -d "$OUT_DIR" ]; then
  mkdir -p "$OUT_DIR"
  cd "$OUT_DIR"
  CMAKE_BUILD_TYPE=Debug
  [ "$BUILD_MODE" = "debug" ] || CMAKE_BUILD_TYPE=Release
  cmake -G Ninja -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE "$SRC_DIR"
else
  cd "$OUT_DIR"
fi

_pidfile_kill() {
  local pidfile="$1"
  # echo "_pidfile_kill $1"
  if [ -f "$pidfile" ]; then
    local pid=$(cat "$pidfile" 2>/dev/null)
    # echo "_pidfile_kill pid=$pid"
    [ -z "$pid" ] || kill $pid 2>/dev/null || true
    rm -f "$pidfile"
  fi
}

RUN_PIDFILE="$PWD/run-${$}.pid"
BUILD_LOCKFILE=$PWD/.build.lock

__atexit() {
  set +e
  _pidfile_kill "$RUN_PIDFILE"
  rm -rf "$BUILD_LOCKFILE"
}
trap __atexit EXIT

_onsigint() {
  echo
  exit
}
trap _onsigint SIGINT

_run_after_build() {
  _pidfile_kill "$RUN_PIDFILE"
  set +e
  pushd "$USER_PWD" >/dev/null
  ( $OPT_RUN &
    echo $! > "$RUN_PIDFILE"
    wait
    rm -rf "$RUN_PIDFILE" ) &
  popd >/dev/null
  set -e
}

_build() {
  local printed_msg=false
  while true; do
    if { set -C; 2>/dev/null >"$BUILD_LOCKFILE"; }; then
      break
    else
      if ! $printed_msg; then
        printed_msg=true
        echo "waiting for build lock..."
      fi
      sleep 0.5
    fi
  done
  ninja "$@"
  local result=$?
  rm -rf "$BUILD_LOCKFILE"
  return $result
}

if ! $OPT_WATCH; then
  _build "$@"
  [ -z "$OPT_RUN" ] || exec $OPT_RUN
  exit 0
fi

if [ -n "$OPT_RUN" ]; then
  RUN_PIDFILE="$PWD/run-${$}.pid"
  __atexit() {
    _pidfile_kill "$RUN_PIDFILE"
  }
  trap __atexit EXIT
fi

FSWATCH_ARGS=()
if [[ "$@" == *"client"* && "$@" != *"server"* ]]; then
  FSWATCH_ARGS=( "$USER_PWD"/client*.cc "$USER_PWD"/protocol*.* )
elif [[ "$@" == *"server"* && "$@" != *"client"* ]]; then
  FSWATCH_ARGS=( "$USER_PWD"/server*.cc "$USER_PWD"/protocol*.* )
else
  FSWATCH_ARGS=( --exclude='.*' --include='\.(c|cc|cpp|h|s|S)$' "$USER_PWD" )
fi

while true; do
  printf "\x1bc"  # clear screen ("scroll to top" style)
  if _build "$@" && [ -n "$OPT_RUN" ]; then
    _run_after_build
  fi
  echo "watching files for changes ..."
  fswatch \
    --one-event \
    --latency=0.2 \
    --extended \
    --recursive \
    "${FSWATCH_ARGS[@]}"
  echo "———————————————————— restarting ————————————————————"
  _pidfile_kill "$RUN_PIDFILE"
done
