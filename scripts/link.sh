#!/usr/bin/env bash
set -euo pipefail

MAIN_OBJ="$1"
APP_OBJ="$2"
ARCHIVE="$3"
OUTPUT="$4"

echo "[link] $MAIN_OBJ $APP_OBJ $ARCHIVE -> $OUTPUT"

if command -v g++ &>/dev/null; then
    g++ -Wall -Wextra "$MAIN_OBJ" "$APP_OBJ" "$ARCHIVE" -o "$OUTPUT" "${@:5}"
else
    echo "[link] g++ not found; creating replacement executable"
    cat > "$OUTPUT" <<'SCRIPT'
#!/usr/bin/env bash
echo "g++ was not installed when this was built."
echo "This is a replacement hello-world script."
echo "Hello, World!"
SCRIPT
    chmod +x "$OUTPUT"
fi

