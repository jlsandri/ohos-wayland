#!/bin/sh
# Fetches the external sources in sources.tsv: each tree at its pinned
# commit, with its patch series applied. Safe to rerun; a tree already at
# its pinned commit plus its patches is left alone.
# Usage: sh app/scripts/fetch_sources.sh [path...]
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"

applied() { # tree patches: the tree's HEAD is the pin plus exactly the series
	[ "$2" = - ] && count=0 || count=$(ls "$2"/*.patch | wc -l)
	[ "$(git -C "$1" rev-list --count "$3..HEAD" 2>/dev/null)" = "$count" ]
}

grep -v '^#' sources.tsv | while IFS='	' read -r path url commit ref licence patches; do
	[ -n "$path" ] || continue
	if [ $# -gt 0 ]; then
		case " $* " in *" $path "*) ;; *) continue ;; esac
	fi
	if [ -d "$path/.git" ] || [ -f "$path/.git" ]; then
		if git -C "$path" merge-base --is-ancestor "$commit" HEAD 2>/dev/null && applied "$path" "$patches" "$commit"; then
			echo "ok      $path"
			continue
		fi
	else
		echo "clone   $path ($ref)"
		git init -q "$path"
		git -C "$path" remote add origin "$url"
	fi
	git -C "$path" remote set-url origin "$url"
	git -C "$path" fetch -q --depth 1 origin "$commit"
	git -C "$path" checkout -q --detach "$commit"
	if [ "$patches" != - ]; then
		git -C "$path" -c user.name=ohos-wayland -c user.email=ohos-wayland@localhost \
			am -q --committer-date-is-author-date "$root/$patches"/*.patch
	fi
	echo "fetched $path"
done
