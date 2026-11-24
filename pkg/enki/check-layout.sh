#!/bin/sh
# Use the actual linker's segment order, including sanitizer-specific segments.
set -eu
/usr/bin/otool -l "$1" | awk '
  $1 == "cmd" { segment = ($2 == "LC_SEGMENT_64"); next }
  segment && $1 == "segname" {
    if ($2 == "__LINKEDIT") { if (previous != "__ENKI") exit 1; found = 1 }
    else if (found) exit 1
    previous = $2
    segment = 0
  }
  END { if (!found) exit 1 }
' || { echo 'enki: __ENKI must be the last segment before __LINKEDIT' >&2; exit 1; }
