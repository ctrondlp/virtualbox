#!/bin/bash
# Regenerates docs/changes.md from git: one row per merged feature branch, grouped by area, with the commit
# count, the files changed and the lines added/removed against upstream main.
#
# Usage (from the repository root):  bash docs/_tools/gen-changes.sh [base-ref]
# base-ref defaults to origin/main.  The branch refs must exist in this repository.
# The area, the status and the order of the rows are in the table below; add a line for a new branch.

BASE=${1:-origin/main}
OUT=docs/changes.md

# area | branch | status (PR draft = written up as an upstream change, local = stays in this fork) | parent branch (optional,
# for a branch that is stacked on another one, so only its own commits are counted) | description (optional, replaces the
# subject of the first commit)
ROWS='
Speed on Hyper-V (NEM) hosts|nem-hv-delivers-ints|PR draft
Speed on Hyper-V (NEM) hosts|nem-win-x2apic|PR draft
Speed on Hyper-V (NEM) hosts|nem-msr-exit-import|PR draft
Speed on Hyper-V (NEM) hosts|vmm-halt-poll|PR draft
Speed on Hyper-V (NEM) hosts|gim-kvm-pv-eoi|PR draft
Speed on Hyper-V (NEM) hosts|nem-win-register-shadowing|PR draft
Speed on Hyper-V (NEM) hosts|nem-win-halt-partial-import|PR draft
Speed on Hyper-V (NEM) hosts|nem-win-gim-xcpt-narrowing|PR draft|nem-lazy-db-exits
Speed on Hyper-V (NEM) hosts|726-nem-timer-kick|PR draft
Speed on Hyper-V (NEM) hosts|iprt-win-ecoqos-optout|PR draft
Speed on Hyper-V (NEM) hosts|iprt-win-highres-waits|local|iprt-win-ecoqos-optout
Correctness fixes|861-x2apic-off-keeps-apic|PR draft
Correctness fixes|930-iem-riprel-cbimm|PR draft
Correctness fixes|932-nem-tpr-low-nibble|PR draft
Correctness fixes|iem-vcvt-256-source-reg|PR draft
Correctness fixes|nem-lazy-db-exits|PR draft
Correctness fixes|nem-win-restore-drx-after-debugging|PR draft
Graphics|virtio-gpu-device|local|fa9e0292a23|A virtio-gpu graphics device: 2D display and Vulkan 3D (Venus) for Linux guests, with the EFI driver, API, GUI and VBoxManage support (see the virtio-gpu page)
Graphics|vmsvga-dx11-gpu-select|PR draft
Graphics|devvga-wide-scanline|PR draft
Guest CPU model|cpum-isa-exts|PR draft
Guest CPU model|cpum-intel-microarchs|PR draft
macOS guests|macos-guest-types|local|macos-cpum-hooks
macOS guests|macos-cpum-hooks|local|macos-efi-apple-features
macOS guests|macos-smc-fixes|local
macOS guests|macos-smc-diagnostics|local|macos-smc-fixes
macOS guests|macos-efi-apple-features|local|macos-smc-diagnostics
macOS guests|macos-efi-apple-loader|local
macOS guests|macos-efi-appleprop-stubs|local
macOS guests|efi-msvc-intrinsics|local
Test tooling (VBoxBFE, the stand-alone front end used for testing)|vboxbfe-config-ctor-rc|local
Test tooling (VBoxBFE, the stand-alone front end used for testing)|vboxbfe-numcpus|local
Test tooling (VBoxBFE, the stand-alone front end used for testing)|vboxbfe-sdl-update-events|local
Test tooling (VBoxBFE, the stand-alone front end used for testing)|vboxbfe-update-clip|local
Test tooling (VBoxBFE, the stand-alone front end used for testing)|vboxbfe-vbva-display|local
Test tooling (VBoxBFE, the stand-alone front end used for testing)|vboxbfe-test-hook|local
'

{
    echo '---'
    echo 'layout: default'
    echo 'title: All changes'
    echo '---'
    echo
    echo '# All changes'
    echo
    echo "Generated from git by \`docs/_tools/gen-changes.sh\` against \`$BASE\`. One row per feature branch that is merged into the"
    echo 'integration branch. **PR draft** means the change is written up as a stand-alone upstream change; **local** means it stays'
    echo 'in this fork. "Size" is the number of commits, files changed and lines added/removed against upstream.'
    echo
    area=
    echo "$ROWS" | while IFS='|' read -r a b st par desc; do
        [ -z "$b" ] && continue
        if ! git rev-parse --verify -q "refs/heads/$b" > /dev/null; then
            echo "skipping $b: no such branch" >&2
            continue
        fi
        if [ "$a" != "$area" ]; then
            [ -n "$area" ] && echo
            echo "## $a"
            echo
            echo '| Branch | What it does | Size | Status |'
            echo '|---|---|---|---|'
            area=$a
        fi
        base=$BASE
        [ -n "$par" ] && base=$par
        # the first commit of the branch describes it best (a later one is often just the changelog line)
        subj=$(git log --reverse --no-merges --format=%s "$base..$b" | head -1 | sed 's/, github:gh-[0-9A-Z]*$//; s/|/\\|/g')
        n=$(git rev-list --count --no-merges "$base..$b")
        stat=$(git diff --shortstat "$base...$b" | sed 's/^ *//')
        [ "$n" = 1 ] && cw=commit || cw=commits
        [ -n "$desc" ] && subj=$desc
        files=$(echo "$stat" | sed -n 's/^\([0-9]*\) files\? changed.*/\1/p')
        [ "${files:-0}" = 1 ] && fw=file || fw=files
        ins=$(echo "$stat" | sed -n 's/.* \([0-9]*\) insertions\?(+).*/\1/p')
        del=$(echo "$stat" | sed -n 's/.* \([0-9]*\) deletions\?(-).*/\1/p')
        echo "| \`$b\` | $subj | ${n} ${cw}, ${files:-0} ${fw}, +${ins:-0} -${del:-0} | $st |"
    done
} > "$OUT"
echo "wrote $OUT"
