# Sliced Labs monorepo gates. Genesis version (WO-000); WO-001 replaces this with the full recipe surface.
set shell := ["bash", "-euo", "pipefail", "-c"]

default:
    @just --list

# gate.fast (FACTORY.md): under 60 s
fast: wo-check

# gate.full (FACTORY.md). Genesis stub: WO-001 ratchets it to real gates.
check: fast
    @echo "check: genesis stub (owner WO-001)"

# every work order carries a `Traces: SL-EP-...` line and passes the factory lint when it is available
wo-check:
    @for f in work-orders/WO-*.md; do grep -qE '^Traces: SL-EP-[0-9]{3}' "$f" || { echo "$f: missing 'Traces: SL-EP-nnn' line"; exit 1; }; done
    @echo "wo-check: ok ($(ls work-orders/WO-*.md | wc -l) work orders)"
