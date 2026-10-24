# Assistant Send and UCT control checkpoint — 2026-10-24

## UCT discovery follow-up

The owner's live Luna request searched quick-mode presets instead of the UCT
settings. Source `d7dad4fc4508324947124b0dc0b7797d3c6185c5` adds shared product
vocabulary and explicit UCT/OCR field guidance for both providers. It also
permits the native Text Clarity master preference on CPU paths, because that
master gates CPU OCR. CUDA enhancement still reports ineffective there;
the other 47 CPU-camera image mutations remain refused.

Final follow-up: **12/245 scoped PASS**, no skipped rows/unresolved results;
**17/17 synthetic** and **9/9 actual-target offscreen V1 SDK scenarios PASS**.
The actual CUDA application build passed. SHA-256:
`9f6468ceff0441538cb085a5fc84920e3e4778a4d5933eb76a65b8f3e368c519`.
Child/controller both exited **0**, 114.326 seconds, final Job active zero,
runner release and independent probe verified, no cleanup issues.

Evidence:
`Z:/Projects/OkuFlow/local-work-data/release-acceptance-20261024/assistant-uct-discovery-r3/C2-20261024-015351-7a59a179643540ad896798bc12e21ba9`.
Earlier discovery `r1` and `r2` failures are preserved: the dependency admission
gap, then obsolete CPU classification/count assertions and a failed fixture's
unretired context. The fixture now always retires that context on assertion
failure. The passing end-to-end row checks actual child instructions/tool schema,
get/set/get success, native enablement, saved intent and ready/effective state
with a private recognizer. The fixture is CPU-only and creates no CUDA surface;
this does not depend on whether the host has a GPU.

Read-only Opus review `opus-uct-discovery-cpu-master-review-20261024-r1`
(same parent command as below) found no P1/P2 in the production repairs at
`f598b2a`; the subsequent commit changes tests/docs only. Nonblocking follow-ups
include a direct HTTP guidance-payload assertion and the Text Clarity reason
on GPU-capable media awaiting its first processed frame (currently it can say
`disabled`). Neither changes UCT's separate backend-readiness check.

The rebuilt app still needs the owner's natural-language Luna retry. These
model-free passes do not establish model reasoning, GPU OCR, spoken
accessibility or installer acceptance.

## Earlier Send checkpoint

Tested source: `f7ccf77fe0e07b747ad98ce119b3e329d5efbe14`.
This is scoped background validation, not installer or full release acceptance.

## Result

- Tracked CPU gate: **12/245 selected tests PASS**, no skipped Qt rows or
  unresolved results. Matching per-test fingerprints could be reused.
- Independent V1 MCP SDK clients: **17/17 synthetic** and **9/9 actual-target
  offscreen scenarios PASS**. Catalog: 100 parameters, 55 image parameters,
  43 tools, 17 protected entries and 46 actions.
- Actual CUDA-enabled `oku_flow` application compiled successfully. Its SHA-256
  is `58a0f9868d9e25d00f3de38557f049e6a983a91e1da3409e53060dbad8887fcd`.
- Outer controller and work child exited **0** in 138.304 seconds; exact Job
  membership reached zero and the independent runner-release probe passed.
- Translation validation: **24 catalogs × 886 finished entries**, exact source
  parity. The two UCT captions received review of all 48 translations; nine
  replacements were applied. This remains machine-assisted language review.

Evidence root:
`Z:/Projects/OkuFlow/local-work-data/release-acceptance-20261024/assistant-flow-r5/C2-20261024-013351-1a999e71a6c547b8991711ad34eaea30`.
It retains stage commands/timings, source/binary pins, the tracked ledger and
per-attempt logs, private SDK receipts, and final cleanup receipt.

## Covered behavior

Native Send prepares first-use tools and continues one submitted request without
requiring Test endpoint or Start conversation clicks. Stop, direct configuration
refresh, a diagnostic click during preparation, refused probes, restored
provider histories and the loopback HTTP success/failure paths are exercised.
Old provider histories remain separate; a logical chat can continue across
embedded turns. All provider responses in these checks are fixtures.

Embedded current-view permission applies to the submitted turn without an image
consent card. Tests retain old-stamp refusal, explicit revocation, source
freshness and independent external MCP/photo/recording permissions. The initial
attachment checkbox does not revoke the turn's observation authority.

UCT enable and Show original use the native controller, persistence and typed
receipts. Requested enablement is distinct from Text Clarity and backend
readiness. Receipt captions match the actual controls and have translations.

## Review and preserved failures

Opus rounds:
`opus-assistant-send-current-view-uct-review-20261024-r1` and
`opus-assistant-send-uct-repair-review-20261024-r2`, both under parent command
`mcp:4e7830f3-085a-4eba-8674-613264588efc`. R2 closed the routing/probe/readiness
findings and identified the final receipt-caption issue. The captions were then
repaired with per-entry review
`uct-checkbox-48-label-translations-review-20261024-r1` and native caption checks.

Earlier `assistant-flow-r1` through `r4` remain preserved: obsolete inventory
counts, an uncataloged caption, an environment-mismatch resume refusal, and
HTTP-fixture naming/credential mistakes. The HTTP fixture's nonempty fake key
hit its deliberately rejecting secret store and affected subsequent saved-state
checks; it now uses a credential-free loopback endpoint and the HTTP tool name.
No failed attempt was relabeled or deleted. Historical exit-125 receipts remain
historical failures; the new zero exit applies only to this block.

## Remaining acceptance

The owner's earlier Luna zoom actions establish earlier live control behavior.
They do not validate these new bytes. Current-view image sharing, actual UCT
startup, spoken screen-reader behavior and native focus/layout still require
the rebuilt app's manual checks. No account-backed inference, real capture,
GPU inference, foreground GUI fixture, MSI installation or publication ran in
this scoped block. Full shipping/CPU/CUDA/bundle and candidate VM acceptance
remain separate release work.
