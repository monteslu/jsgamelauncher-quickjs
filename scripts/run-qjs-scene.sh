#!/usr/bin/env bash
# Stage a bench scene into a self-contained dir and run it under jsglq.
# Scenes import ../../harness.js and ../lib/*, so that layout must be preserved.
#
# The options object is written with printf rather than a heredoc: an unquoted
# heredoc mangles the JSON braces, and the harness then receives a STRING where it
# expects an object. readOptions() silently falls back to defaults in that case, so
# the scene runs the wrong length with no error at all.
set -euo pipefail
SCENE="$1"; shift
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="${TMPDIR:-/tmp}/jsglq-scene-$SCENE"
rm -rf "$STAGE"; mkdir -p "$STAGE/scenes/$SCENE" "$STAGE/scenes/lib/vendor"
cp "$ROOT/bench/harness.js" "$STAGE/"
cp "$ROOT"/bench/scenes/lib/*.js "$STAGE/scenes/lib/" 2>/dev/null || true
cp "$ROOT"/bench/scenes/lib/vendor/*.js "$STAGE/scenes/lib/vendor/" 2>/dev/null || true
cp "$ROOT/bench/scenes/$SCENE/main.js" "$STAGE/scenes/$SCENE/"
OPTS="${BENCH_OPTS:-}"
if [ -z "$OPTS" ]; then
  OPTS='{"mode":"capped","frames":600,"width":960,"height":540}'
fi
printf 'globalThis.__BENCH_OPTS__ = %s;\n' "$OPTS" > "$STAGE/main.js"
printf "import('./scenes/%s/main.js').catch(e => console.log('SCENE FAILED:', e.message, e.stack));\n" "$SCENE" >> "$STAGE/main.js"
exec "$ROOT/build/jsglq" --headless "$@" "$STAGE"
