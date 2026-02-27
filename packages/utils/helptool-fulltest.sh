#!/bin/bash
# helptool-fulltest.sh -- Test .lsh files through the actual helptool parsing pipeline
#
# Loads each file as a help-book and tries to render every entry,
# catching any parse errors (splice-ul crash, tag mixing, etc.)
#
# Usage:
#   bash helptool-fulltest.sh                    # test all Claude packages
#   bash helptool-fulltest.sh path/to/file.lsh   # test a single file

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PACKAGES_DIR="$(dirname "$SCRIPT_DIR")"
LUSH="${LUSH:-lush}"

# Helper: test a single .lsh file through the help-book pipeline
test_one_file() {
    local f="$1"
    local fname="$(basename "$f")"

    # Run Lush in a subshell, loading the file as a help-book
    # and iterating through all nodes to trigger parsing
    output=$(HOME=/tmp/claude-1000 "$LUSH" -e "
(libload \"libstd/help\")

;; Walk a help-node tree depth-first, calling getdata on each node
(de walk-help-tree (node depth)
  (let ((data (==> node getdata)))
    (printf \"  [%d] %s\\n\" depth :node:name) )
  (let ((children (==> node getchildren)))
    (when children
      (each ((c children))
        (walk-help-tree c (1+ depth)) ) ) ) )

;; Load the file and test all entries
(let ((book (new help-book \"$f\" \"test\")))
  (walk-help-tree :book:root 0)
  (printf \"OK\\n\") )
" 2>&1)

    local rc=$?
    if [ $rc -ne 0 ] || echo "$output" | grep -q '^\*\*\*'; then
        echo "FAIL: $f"
        echo "$output" | grep '^\*\*\*' | head -3
        return 1
    else
        echo "  OK: $fname ($(echo "$output" | grep -c '^\s*\[') entries)"
        return 0
    fi
}

# Collect files to test
files=()
if [ $# -gt 0 ]; then
    # Test specific files
    for f in "$@"; do
        files+=("$(realpath "$f")")
    done
else
    # Test all Claude packages
    for pkg in sqlite timedate datatable columnardb json httpd wire libuv mapper curl lz4 csvread; do
        pkgdir="$PACKAGES_DIR/$pkg"
        if [ -d "$pkgdir" ]; then
            for f in "$pkgdir"/*.lsh; do
                [ -f "$f" ] && files+=("$f")
            done
        fi
    done
fi

echo "=== Helptool Full Pipeline Test ==="
echo "Testing ${#files[@]} files..."
echo ""

pass=0
fail=0
skip=0

for f in "${files[@]}"; do
    # Only test files that have #? entries
    if grep -q '^#?' "$f" 2>/dev/null; then
        if test_one_file "$f"; then
            ((pass++))
        else
            ((fail++))
        fi
    else
        ((skip++))
    fi
done

echo ""
echo "=== Results: $pass passed, $fail failed, $skip skipped (no #? entries) ==="
exit $fail
