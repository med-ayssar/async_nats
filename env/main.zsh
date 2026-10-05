# Source from this repository: source env/main.zsh
# Sets CLANGXX to the Nix clang++ that ./build.sh records in compile_commands.json.
# Also exports NIX_CFLAGS_COMPILE and the wrapper host role from that shell.
# clangd runs CLANGXX to learn its default headers. The wrapper applies Boost
# and spdlog only when that role is set. Start Neovim from this shell.

if [[ "${ZSH_EVAL_CONTEXT:-}" != *:file ]]; then
  print -u2 "Source this file: source env/main.zsh"
  return 1 2>/dev/null || exit 1
fi

env_file=${${(%):-%x}:A}
repo=${env_file:h:h}

shell_env=$(
  cd "$repo" && nix develop ".#async-nats-clang" -c bash -c '
    printf "CLANGXX %s\n" "$(command -v clang++)"
    printf "CFLAGS %s\n" "$NIX_CFLAGS_COMPILE"
    for name in ${!NIX_CC_WRAPPER_TARGET_HOST_*}; do
      printf "HOST %s %s\n" "$name" "${!name}"
    done
  '
)

CLANGXX=""
NIX_CFLAGS_COMPILE=""
while IFS= read -r line; do
  case "$line" in
  CLANGXX\ *)
    CLANGXX=${line#CLANGXX }
    ;;
  CFLAGS\ *)
    NIX_CFLAGS_COMPILE=${line#CFLAGS }
    ;;
  HOST\ *)
    rest=${line#HOST }
    name=${rest%% *}
    value=${rest#* }
    export "$name=$value"
    ;;
  esac
done <<< "$shell_env"

if [[ -z "$CLANGXX" || ! -x "$CLANGXX" ]]; then
  print -u2 "Nix clang++ was not found. Run this from the async_nats checkout."
  return 1
fi

export CLANGXX
export NIX_CFLAGS_COMPILE
print "CLANGXX=$CLANGXX"
