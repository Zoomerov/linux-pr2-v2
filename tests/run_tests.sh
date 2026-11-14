#!/usr/bin/env sh
set -eu
binary=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

for package in A B C; do
    "$binary" --test --package "$package" \
        --repository "$root/tests/repository.txt" \
        --output "$tmp/$package.png" --order >"$tmp/$package.log"
    test -s "$tmp/$package.png"
    test -s "$tmp/$package.d2"
    test "$(od -An -tx1 -N8 "$tmp/$package.png" | tr -d ' \n')" = 89504e470d0a1a0a
done

grep -q '"A" -> "B"' "$tmp/A.d2"
grep -q '"A" -> "C"' "$tmp/A.d2"
grep -q '"B" -> "D"' "$tmp/A.d2"
grep -q '"C" -> "D"' "$tmp/A.d2"
sed -n '/Loading order:/,/direction:/p' "$tmp/A.log" | \
    grep -A4 'Loading order:' >"$tmp/order"
printf 'Loading order:\nD\nB\nC\nA\n' >"$tmp/expected"
cmp "$tmp/expected" "$tmp/order"

"$binary" --test --package E --repository "$root/tests/repository.txt" \
    --output "$tmp/cycle.png" >"$tmp/cycle.log"
test -s "$tmp/cycle.png"
if "$binary" --test --package E --repository "$root/tests/repository.txt" \
    --output "$tmp/cycle-order.png" --order >"$tmp/invalid.log" 2>&1; then
    echo 'Expected cycle detection' >&2
    exit 1
fi
grep -q 'Dependency cycle' "$tmp/invalid.log"

if "$binary" --test --package a --repository "$root/tests/repository.txt" \
    --output "$tmp/invalid.png" >"$tmp/invalid.log" 2>&1; then
    echo 'Expected package validation' >&2
    exit 1
fi
grep -q 'only A-Z' "$tmp/invalid.log"

if "$binary" --test --package A --repository "$tmp/missing.txt" \
    --output "$tmp/invalid.png" >"$tmp/invalid.log" 2>&1; then
    echo 'Expected repository validation' >&2
    exit 1
fi
grep -q 'file not found' "$tmp/invalid.log"

if "$binary" --test --package A --repository "$root/tests/repository.txt" \
    --output "$tmp/invalid.svg" >"$tmp/invalid.log" 2>&1; then
    echo 'Expected output validation' >&2
    exit 1
fi
grep -q 'must end in .png' "$tmp/invalid.log"

if "$binary" --package A --repository "$root/tests/repository.txt" \
    --output "$tmp/invalid.png" >"$tmp/invalid.log" 2>&1; then
    echo 'Expected mode validation' >&2
    exit 1
fi
grep -q 'must be an HTTP URL' "$tmp/invalid.log"

if "$binary" --test=yes --package A --repository "$root/tests/repository.txt" \
    --output "$tmp/invalid.png" >"$tmp/invalid.log" 2>&1; then
    echo 'Expected test mode validation' >&2
    exit 1
fi
grep -q 'Unknown option' "$tmp/invalid.log"

if "$binary" --test --test --package A --repository "$root/tests/repository.txt" \
    --output "$tmp/invalid.png" >"$tmp/invalid.log" 2>&1; then
    echo 'Expected duplicate option validation' >&2
    exit 1
fi
grep -q 'Duplicate option' "$tmp/invalid.log"

echo 'All tests passed'
