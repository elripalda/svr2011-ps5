#!/usr/bin/env bash
# [ps5 port] Bring the PS5 port up to a new release of the PC port
# (KaikoClanworth1/wwe-svr2011-pc): its game code into this branch, and its
# SDK patch (port/patches/rexglue-sdk-svr2011.patch) into the PS5 runtime.
# Online needs the same version on both sides, so this is run for every PC
# release.
#
#   port/tools/ps5_update_upstream.sh <tag> <rexglue-ps5 checkout> [build dir] [ps5-plume-triangle checkout]
#
#   e.g. port/tools/ps5_update_upstream.sh v2.1.4 ../rexglue-ps5
#
# With the last two it also builds and packages (ps5_package.sh).
# Both checkouts must be clean. When a merge needs a hand it stops and lists
# the files: fix them, commit, and run it again - it carries on from there.
set -euo pipefail

tag=${1:?tag of the PC release, e.g. v2.1.4}
rt=$(realpath "${2:?the rexglue-ps5 runtime checkout}")
build=${3:-}
tri=${4:-}
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
upstream_url=https://github.com/KaikoClanworth1/wwe-svr2011-pc
sdk_url=https://github.com/rexglue/rexglue-sdk

g() { git -C "$repo" "$@"; }
r() { git -C "$rt" "$@"; }
clean() { [[ -z $(git -C "$1" status --porcelain --untracked-files=no --ignore-submodules=all) ]]; }
stop() { echo "$*" >&2; exit 1; }

clean "$repo" || stop "$repo has changes that aren't committed - commit or set them aside first"
clean "$rt" || stop "$rt has changes that aren't committed - commit or set them aside first"
[[ -z $(g diff --name-only --diff-filter=U) ]] || stop "$repo is in the middle of a merge - finish it first"

# -- the PC port's code ------------------------------------------------------

# Its tags are kept apart from this repository's own: upstream/tags/<tag>.
if ! g remote get-url upstream >/dev/null 2>&1; then
    g remote add upstream "$upstream_url"
    g config remote.upstream.tagOpt --no-tags
    g config --replace-all remote.upstream.fetch '+refs/heads/main:refs/remotes/upstream/main'
    g config --add remote.upstream.fetch '+refs/tags/*:refs/remotes/upstream/tags/*'
fi
echo "==> fetching the PC port"
g fetch -q upstream
new=$(g rev-parse --verify -q "upstream/tags/$tag^{commit}") || stop "the PC port has no release $tag"
old_version=$(head -1 "$repo/port/VERSION.txt" | tr -d '[:space:]')
old=$(g rev-parse --verify -q "upstream/tags/v$old_version^{commit}" || true)

# README.md is the PS5 one: a merge keeps it, and README-PC.md gets theirs.
g config merge.ours.driver true
if g merge-base --is-ancestor "$new" HEAD; then
    echo "==> the game code already has $tag"
else
    echo "==> merging the PC port's $tag (was v$old_version)"
    if ! g merge --no-edit -m "Merge KaikoClanworth1/wwe-svr2011-pc $tag" "$new"; then
        echo
        echo "Both sides changed these files - they need a hand:"
        g diff --name-only --diff-filter=U | sed 's/^/    /'
        echo
        echo "Keep the PS5 parts (marked [ps5 port] or under __PROSPERO__) and the PC port's new code,"
        echo "then: git add <files> && git commit --no-edit, and run this again."
        exit 3
    fi
fi
if ! g show "$new:README.md" | cmp -s - "$repo/README-PC.md"; then
    g show "$new:README.md" > "$repo/README-PC.md"
    g add README-PC.md
    g commit -q -m "README-PC.md: the PC port's $tag README"
fi

# -- the PS5 runtime ---------------------------------------------------------

# rexglue-pc: the stock ReXGlue SDK with the PC port's SDK patch, one commit
# per PC release; the PS5 runtime branch merges it.
r rev-parse --verify -q refs/heads/rexglue-pc >/dev/null || r fetch -q origin rexglue-pc:rexglue-pc ||
    stop "the runtime checkout has no rexglue-pc branch (it's on GitHub: git fetch origin rexglue-pc:rexglue-pc)"
base=$(g show "$new:README.md" | grep -oE 'rexglue-sdk checkout [0-9a-f]{40}' | head -1 | awk '{print $3}')
[[ -n $base ]] || stop "couldn't find the ReXGlue SDK commit in the PC port's README"
if ! r cat-file -e "$base^{commit}" 2>/dev/null; then
    echo "==> fetching ReXGlue SDK ${base:0:7}"
    r fetch -q --no-tags "$sdk_url" "$base"
fi
patch=$(mktemp)
index=$(mktemp -u)
trap 'rm -f "$patch" "$index"' EXIT
g show "$new:port/patches/rexglue-sdk-svr2011.patch" > "$patch"
GIT_INDEX_FILE=$index r read-tree "$base"
GIT_INDEX_FILE=$index r apply --cached --whitespace=nowarn "$patch" || stop "the PC port's SDK patch doesn't apply to ReXGlue ${base:0:7}"
tree=$(GIT_INDEX_FILE=$index r write-tree)
pc=$(r rev-parse rexglue-pc)
sdk_moved=0
if [[ $tree != $(r rev-parse "$pc^{tree}") ]]; then
    parents=(-p "$pc")
    if ! r merge-base --is-ancestor "$base" "$pc"; then
        parents+=(-p "$base")
        sdk_moved=1
    fi
    commit=$(r commit-tree "$tree" "${parents[@]}" -m "The PC port's SDK patch from wwe-svr2011-pc $tag

port/patches/rexglue-sdk-svr2011.patch from KaikoClanworth1/wwe-svr2011-pc
$tag (${new:0:7}), applied to rexglue-sdk ${base:0:7}.")
    r update-ref refs/heads/rexglue-pc "$commit" "$pc"
    echo "==> rexglue-pc: the SDK patch of $tag"
else
    echo "==> the SDK patch didn't change"
fi
if r merge-base --is-ancestor rexglue-pc HEAD; then
    echo "==> the runtime already has it"
else
    echo "==> merging it into the runtime ($(r rev-parse --abbrev-ref HEAD))"
    if ! r merge --no-edit -m "Merge the PC port's $tag SDK patch into the PS5 runtime" rexglue-pc; then
        echo
        echo "Both sides changed these runtime files - they need a hand:"
        r diff --name-only --diff-filter=U | sed 's/^/    /'
        echo
        echo "Fix them in $rt, git add + git commit --no-edit there, and run this again."
        exit 3
    fi
fi

# -- what else the release needs ----------------------------------------------

if [[ -n $old ]] && ! g diff --quiet "$old" "$new" -- port/svr2011_config.toml port/svr2011_manifest.toml; then
    echo
    echo "!! The PC port changed svr2011_config.toml / svr2011_manifest.toml: generate the game's code again"
    echo "   (rexglue codegen, then port/tools/ps5_patch_generated.py generated/rexglue.cmake) before building."
fi
if (( sdk_moved )); then
    echo
    echo "!! The PC port moved to ReXGlue SDK ${base:0:7}: generate the game's code again with that"
    echo "   release's rexglue tool, then port/tools/ps5_patch_generated.py."
fi
if [[ -n $old ]] && ! g diff --quiet "$old" "$new" -- port/patches/plume-svr2011.patch; then
    echo
    echo "!! plume-svr2011.patch changed: rebuild plume from d723793 with it and then plume-ps5.patch."
fi
echo
echo "Bundled mods: put the .svrmod files of $upstream_url/releases/download/$tag/SvR2011-Mods-$tag.zip"
echo "(its Bundled Mods folder) in the app's Bundled Mods folder - the app installs the changed ones at its next start."

if [[ -n $build && -n $tri ]]; then
    echo
    echo "==> building"
    cmake "$build" >/dev/null
    ninja -C "$build" svr2011
    bash "$repo/port/tools/ps5_package.sh" "$build" "$tri"
fi
echo
echo "Done: the PS5 port is on the PC port's $tag ($(head -1 "$repo/port/VERSION.txt"))."
