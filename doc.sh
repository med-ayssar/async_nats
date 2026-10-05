#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}/docs/site"

# .#doc is the documentation derivation: Node.js plus the doc command.
# The command installs the Astro site, builds it, and serves it.
exec nix shell "${SCRIPT_DIR}#doc" -c doc "$@"
