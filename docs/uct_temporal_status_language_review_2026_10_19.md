# UCT temporal pause status language review

The new `uct.pausedExtraStable` status source is:

> Paused while Extra Stable is on. The original camera image is shown.

It describes an unsupported stabilized composite, not a paused video. Supported
Extra Stable captures now carry their exact held acquisition; they do not pause
UCT merely because the feature is enabled. Recovery blends remain ineligible.
Temporal Smooth alone no longer causes this status. Each target preserves the
existing language's Extra Stable control name. CPU debug composites use their
own text-recognition pause reason rather than claiming Extra Stable is active.

A read-only GPT-6-Luna subagent reviewed all 24 new target strings against the
English source, UI context and existing feature-name translations. All received
OK; none were flagged or unread. The coordinating agent checked coverage against
the actual catalogs. Structural validation passed with 875 complete source keys
per catalog. This is machine-assisted linguistic review, not native-speaker,
rendered RTL/font or Windows screen-reader acceptance.

The independent Opus review also identified two missing status keys. Luna reviewed
all 48 language/key combinations for `uct.originalCameraText` and
`uct.pausedDebugComposite`. The 24 original-camera targets were OK. All initial
debug-pause targets were flagged for possibly implying that video playback was
paused. The source now explicitly says **Text recognition is paused in the debug
composite view. The original camera image is shown.** The coordinator applied the
reviewer's 23 replacements and a natural Turkish word-order adjustment.

Luna re-reviewed every final debug target: 23 were OK; Persian was flagged for
not making the temporary pause explicit. The exact recommended Persian correction
was applied. The 24 original-camera targets remained byte-identical to the first
review. Coverage is 72 reviewed language/key combinations across the three new
keys; none are unread. Structural validation now covers 877 keys per catalog.
This review does not establish native speaker, rendered UI or screen-reader acceptance.

| Language | Key | Verdict |
| --- | --- | --- |
| tr | uct.pausedExtraStable | OK |
| de | uct.pausedExtraStable | OK |
| fr | uct.pausedExtraStable | OK |
| es | uct.pausedExtraStable | OK |
| it | uct.pausedExtraStable | OK |
| pt_BR | uct.pausedExtraStable | OK |
| ru | uct.pausedExtraStable | OK |
| uk | uct.pausedExtraStable | OK |
| ar | uct.pausedExtraStable | OK |
| hi | uct.pausedExtraStable | OK |
| ja | uct.pausedExtraStable | OK |
| ko | uct.pausedExtraStable | OK |
| zh_Hans | uct.pausedExtraStable | OK |
| nl | uct.pausedExtraStable | OK |
| pl | uct.pausedExtraStable | OK |
| cs | uct.pausedExtraStable | OK |
| sv | uct.pausedExtraStable | OK |
| id | uct.pausedExtraStable | OK |
| vi | uct.pausedExtraStable | OK |
| th | uct.pausedExtraStable | OK |
| bn | uct.pausedExtraStable | OK |
| fa | uct.pausedExtraStable | OK |
| he | uct.pausedExtraStable | OK |
| ur | uct.pausedExtraStable | OK |
