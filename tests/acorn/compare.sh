#!/bin/bash
# compare.sh SCRIPT.js ... - dmvs_js's tree of each script against acorn's.
#
# Needs node with acorn (npm install acorn) and a host build of dmvs_js;
# BUILD (default: ../../build) is where its dmf/ is.
here=$(cd "$(dirname "$0")" && pwd)
build=${BUILD:-$here/../../build}
status=0
for js in "$@"; do
    ref=$(mktemp) mine=$(mktemp)
    node "$here/tree.js" "$js" > "$ref" || { echo "$js: acorn cannot parse it"; status=1; continue; }
    DMOD_DMF_DIR="$build/dmf" dmod_loader "$build/dmf/jsdump.dmf" --args -o "$mine" "$js" > /dev/null 2>&1
    if cmp -s "$ref" "$mine"; then
        echo "$js: same ($(stat -c %s "$ref") bytes)"
    else
        echo "$js: DIFFERENT"
        cmp "$ref" "$mine" | head -1
        status=1
    fi
    rm -f "$ref" "$mine"
done
exit $status
