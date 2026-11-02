#!/bin/sh
#
# check-version.sh — fail a pull request that changes the code or the build
# but not _VER in src/main.c.  A new version must be
# MAJOR.MINOR.PATCH without leading zeros, greater than the base one, and
# have its section in CHANGELOG.md ("## <version> — YYYY-MM-DD") that the
# base branch does not have yet.  src/, ci/ and the build workflow
# (.github/workflows/build.yml) count as code or build, without the Markdown files in them; docs, tests, screenshots and
# the rest of .github/ (this check and its workflow, version.yml, included)
# need no new version.  NO_RELEASE=true (the pull request's `no-release`
# label) lets a change to the code or the build through without a new
# version, for changes that leave the release files as they are (a test or
# publication step, a build-script tidy-up, a comment).
#
# Usage: .github/check-version.sh [BASE]   (default origin/main; CI passes
# HEAD^1, the base side of the pull request's merge commit)
#

set -eu

base=${1:-origin/main}
header=src/main.c
code="src ci .github/workflows/build.yml"

# The one _VER definition. Every "#define _VER" counts, however it is
# spaced ("# define" too), and it must be the plain form
# '#define _VER "X.Y.Z"' (space or tab) that the release job reads.
ver() {
	h=$(cat)
	n=$(printf '%s\n' "$h" | grep -cE '^[[:space:]]*#[[:space:]]*define[[:space:]]+_VER([[:space:]]|$)' || true)
	v=$(printf '%s\n' "$h" | sed -n 's/^#define[[:space:]]\{1,\}_VER[[:space:]]\{1,\}"\([^"]*\)"[[:space:]]*$/\1/p')
	if [ "$n" -ne 1 ] || [ -z "$v" ]; then
		echo "::error::$1:$header: expected one '#define _VER \"X.Y.Z\"'," \
			"found $n definition(s)" >&2
		exit 1
	fi
	printf '%s\n' "$v"
}

new=$(ver HEAD < "$header")
old=$(git show "$base:$header" | ver "$base")

# shellcheck disable=SC2086 # $code is a list of paths
changed=$(git diff --name-only "$base"...HEAD -- $code ':(exclude,glob)**/*.md')

if [ "$new" = "$old" ]; then
	if [ -n "$changed" ] && [ "${NO_RELEASE:-}" = true ]; then
		echo "::notice::code or build changed, _VER stays $old: the no-release" \
			"label says the release files do not change"
		echo "$changed"
		exit 0
	fi
	if [ -n "$changed" ]; then
		echo "::error::code or build changed, _VER is still $old:" \
			"bump PATCH (fix) or MINOR (feature) and add its CHANGELOG.md section," \
			"or label the pull request no-release if the release files do not change"
		echo "$changed"
		exit 1
	fi
	echo "version $new unchanged, no code or build changes"
	exit 0
fi

if ! echo "$new" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'; then
	echo "::error::_VER \"$new\" is not MAJOR.MINOR.PATCH"
	exit 1
fi

top=$(printf '%s\n%s\n' "$old" "$new" | sort -V | tail -n 1)
if [ "$top" != "$new" ]; then
	echo "::error::_VER $new is not greater than $old"
	exit 1
fi

# $new is digits and dots only (checked above): escape the dots, require a date.
re=$(printf '%s' "$new" | sed 's/\./\\./g')
if ! grep -Eq "^## $re — [0-9]{4}-[0-9]{2}-[0-9]{2}\$" CHANGELOG.md; then
	echo "::error::CHANGELOG.md has no section \"## $new — YYYY-MM-DD\""
	exit 1
fi

# The section must come with this pull request, not be one the base
# branch already has (a version taken from there brings no notes).
if git show "$base:CHANGELOG.md" 2>/dev/null | grep -Eq "^## $re — "; then
	echo "::error::the section \"## $new\" is already on $base: take a new version"
	exit 1
fi

echo "version $old -> $new, CHANGELOG.md section found"
