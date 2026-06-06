#!/bin/bash
# =============================================================================
# render_diagrams.sh — Render all Mermaid diagrams to SVG + PNG
# =============================================================================
# Produces both SVG (for README/docs embedding) and PNG (for PDF/print).
#
# Prerequisites:
#   npm install -g @mermaid-js/mermaid-cli
#
# Usage:
#   bash docs/diagrams/render_diagrams.sh
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/rendered"
mkdir -p "$OUT_DIR"

# Use npx to invoke mmdc (handles global install path issues)
MMDC="npx -y @mermaid-js/mermaid-cli"

# Quick sanity check
if ! $MMDC --version &> /dev/null; then
    echo "ERROR: Cannot run mermaid-cli via npx."
    echo "  Install Node.js 18+ and try again."
    exit 1
fi

# Mermaid config for dark theme (matches the project aesthetic)
cat > "$SCRIPT_DIR/.mermaid-config.json" << 'JSON_EOF'
{
  "theme": "dark",
  "themeVariables": {
    "primaryColor": "#6366f1",
    "primaryTextColor": "#e2e8f0",
    "primaryBorderColor": "#818cf8",
    "lineColor": "#94a3b8",
    "secondaryColor": "#1e293b",
    "tertiaryColor": "#0f172a",
    "background": "#0f172a",
    "mainBkg": "#1e293b",
    "nodeBorder": "#6366f1",
    "clusterBkg": "#1e293b",
    "clusterBorder": "#334155",
    "titleColor": "#f1f5f9",
    "edgeLabelBackground": "#1e293b",
    "fontSize": "14px"
  }
}
JSON_EOF

echo "=== IICPC Diagram Renderer ==="
echo "  Source: $SCRIPT_DIR/*.mmd"
echo "  Output: $OUT_DIR/"
echo ""

TOTAL=0
FAILED=0

for mmd_file in "$SCRIPT_DIR"/*.mmd; do
    basename=$(basename "$mmd_file" .mmd)
    echo -n "  Rendering $basename..."

    # SVG (for web/README — vector, sharp at any zoom)
    if $MMDC -i "$mmd_file" \
            -o "$OUT_DIR/${basename}.svg" \
            -c "$SCRIPT_DIR/.mermaid-config.json" \
            -b transparent \
            --quiet 2>/dev/null; then
        echo -n " [SVG ✓]"
    else
        echo -n " [SVG ✗]"
        ((FAILED++))
    fi

    # PNG (for PDF/print/slides — 2x resolution)
    if $MMDC -i "$mmd_file" \
            -o "$OUT_DIR/${basename}.png" \
            -c "$SCRIPT_DIR/.mermaid-config.json" \
            -b transparent \
            -s 2 \
            --quiet 2>/dev/null; then
        echo " [PNG ✓]"
    else
        echo " [PNG ✗]"
        ((FAILED++))
    fi

    ((TOTAL++))
done

echo ""
echo "=== Done: $TOTAL diagrams rendered ($FAILED failures) ==="
echo ""
echo "Files in $OUT_DIR/:"
ls -la "$OUT_DIR"/ 2>/dev/null || true
echo ""
echo "To embed in README.md:"
echo '  ![System Architecture](docs/diagrams/rendered/01_system_architecture.svg)'
echo '  ![Data Flow](docs/diagrams/rendered/02_data_flow.svg)'
echo '  ...'
