#!/usr/bin/env bash
#
# build-selfextract.sh — wrap a NeoZygisk module .zip into a self-extracting
# shell script (`.sh`) that embeds the zip and installs it on-device.
#
# The produced `.sh` (a Magisk/KSU/APatch-style self-extracting installer)
# locates the embedded archive by a unique byte-offset marker, extracts it to a
# temp file, then runs the correct install command for whichever manager is
# present (APatch -> KSU -> Magisk). If no manager is found the module zip is
# left in /data/local/tmp for manual installation.
#
# Usage:
#   build-selfextract.sh <module.zip> <output.sh>
#
# Exit code 0 on success, non-zero on failure.

set -euo pipefail

ZIP="$1"
OUT="$2"

if [[ ! -f "$ZIP" ]]; then
    echo "error: input zip not found: $ZIP" >&2
    exit 1
fi

# The on-device marker must be unique and must not appear inside the zip.
MARKER='##NZ_MODULE##'

HEADER=$(mktemp)
trap 'rm -f "$HEADER"' EXIT

cat > "$HEADER" <<'EOF'
#!/system/bin/sh
# NeoZygisk self-extracting module installer.
# Run with:  sh <this-file>.sh
# Installs via the first supported manager found: APatch -> KernelSU -> Magisk.

# Note: built via concatenation so the literal marker string `##NZ_MODULE##`
# does NOT appear in this header body — it must only occur once, on the final
# appended line right before the embedded archive (see below).
MARKER='##NZ_''MODULE##'
TZIP=/data/local/tmp/neozygisk-module.zip

# Byte offset of the marker. `grep -abo` is POSIX-ish and available in toybox
# (Android) and GNU grep alike; `-b` gives the byte offset. We take the LAST
# occurrence (`tail -n1`) which is the appended marker on the final header line,
# never the one in the body.
OFFSET=$(grep -abF "$MARKER" "$0" | tail -n1 | cut -d: -f1)

if [ -z "$OFFSET" ]; then
    echo "Self-extract marker not found; aborting." >&2
    rm -f "$TZIP"
    exit 1
fi

# The archive starts right after the marker line. `tail -c +N` is 1-based while
# grep's `-b` offset is 0-based, so the zip's first byte is at:
#   0-based marker start (OFFSET) + 1 (to 1-based) + marker length + 1 newline
START=$((OFFSET + ${#MARKER} + 2))
tail -c +"$START" "$0" > "$TZIP"

if command -v apd >/dev/null 2>&1; then
    apd module install "$TZIP"
elif command -v ksud >/dev/null 2>&1; then
    ksud module install "$TZIP"
elif command -v magisk >/dev/null 2>&1; then
    magisk --install-module "$TZIP"
else
    echo "No supported manager (APatch/KernelSU/Magisk) found."
    echo "Module zip left at: $TZIP"
    echo "Install manually with: magisk --install-module $TZIP"
    exit 0
fi

rm -f "$TZIP"
exit $?
EOF

# Finalize the header: ensure it ends with the marker on its own line.
printf '%s\n' "$MARKER" >> "$HEADER"

# Assemble: header script + raw zip bytes.
cat "$HEADER" "$ZIP" > "$OUT"
chmod +x "$OUT"

echo "Wrote self-extracting module: $OUT"
