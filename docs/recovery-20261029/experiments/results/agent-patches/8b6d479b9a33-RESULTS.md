# Human-readable retained width comparison

All figures below compare fixed N8 with bucketed N8 on identical frozen sources,
model/settings/GPU/executable and ordered crop hashes. Nine paired jobs per
source repeat three rounds of three passes; these are not independent images.

| Source | Candidate edits / fixed words | Baseline edit rate | Common identical occurrence max edge drift |
| --- | ---: | ---: | ---: |
| Video55s |13/266 |4.8872% |7.0px |
| Video89s |12/249 |4.8193% |7.4925px |
| Old dense |6/178 |3.3708% |4.0323px |

Every paired job passes the newly authorized5%text side policy. All27fail the
unchanged positional strict and10px+exact-structure policies; those verdicts
remain in the report. The common-word spatial maximum excludes unmatched words
and does not convert the old policy into a pass.

55s meaningful text/grouping changes include `WULKE e`→`WOIKE`,
`8D9C465`→`8D9C465142`, `0/6`→`0/8`, `Letwatch`→`Let watch`, and an uncertain
`Plnte` becoming candidate`Pinte`. Short glyph-like strings also change:
`S`/`5`/`G`, `x`/`×`, and some symbols become protected omissions. These labels
come from recognized metadata; no pixel/source truth is asserted for the icons.
The36px worst positional comparison pairs `Revoke` with `watch` because the new
split inserts another word. `Revoke` itself rematches to the next actual index.
Worst matched common word`All` changes its fitted width15→22px (7px edge drift).

89s changes include `WOKe`→`WOrke`, `/10`→`/`+`10`, `S`→`5`, `x`→`+`,
case/symbol shifts and candidate/protected threshold changes. The18px original
pair compares`ligh` with`First`; both words rematch their source occurrence after
`/10` splits. The maximum common occurrence is short text`e`; fitted bounds stay
identical, with7.4925px in its protected/search envelope.

Old dense changes `roles..`→`roles...`, plus short glyph-like strings `X`→`+`,
an arrow becoming candidate, and two`0`words/`十` becoming protected omissions.
Common-word drift max4.0323px is a`0`occurrence. All unmatched all-word and
candidate-only lists, including their boxes/dispositions, are retained.

Known corpus has ZERO candidate token differences (sparse13,long106,critical81
candidate words). Literal sidecar WER is unchanged: sparse0/13=0%;long1/105=
0.95238%;critical5/80=6.25%. Signed and absolute WER delta is0percentagepoints
for every observation. Critical text/dispositions do not change; this does not
assert the existing five truth errors are correct or repaired.

Root's warmed timing observations are source dependent: video55/89 recognizer
work improves, while long/critical/full-width workloads can regress. The tool
retains raw worker/stage/shape-count/padding/capacity evidence and paired round
medians rather than inferring universal speedup from padding. No3050or120FPS
measurement was made by this offline tool.
