# Original independent read-only crop review

Client request: owned-bf16-crops-readonly-review-20261023-r1
Task ID: node:delegated-task:command%3Amcp%3Aa7267bed-787d-4da3-9a82-31cb240cfc79%3Adelegate-task%3Aowned-bf16-crops-readonly-review-20261023-r1
Child thread ID: thread:delegated-task:command%3Amcp%3Aa7267bed-787d-4da3-9a82-31cb240cfc79%3Adelegate-task%3Aowned-bf16-crops-readonly-review-20261023-r1
Child run ID: run:thread:thread%3Adelegated-task%3Acommand%253Amcp%253Aa7267bed-787d-4da3-9a82-31cb240cfc79%253Adelegate-task%253Aowned-bf16-crops-readonly-review-20261023-r1:ordinal:1
Child node ID: node:delegated-task:command%3Amcp%3Aa7267bed-787d-4da3-9a82-31cb240cfc79%3Adelegate-task%3Aowned-bf16-crops-readonly-review-20261023-r1
Status: completed; no pending children.

**8/8 crops reviewed: 8 OK, 0 flagged, 0 unread.** One documentation correction remains. No generator, byte-integrity or static contract mismatch found.

**Flagged — incorrect current artifact location:** [crop documentation line 20](Z:/Projects/T3-worktrees/oku_flow/t3-1e42a08c/docs/uct_owned_bf16_crops.md:20), [path documentation line 7](Z:/Projects/T3-worktrees/oku_flow/t3-1e42a08c/docs/hardcoded_paths.md:7), and [license documentation line 6](Z:/Projects/T3-worktrees/oku_flow/t3-1e42a08c/docs/THIRD_PARTY_LICENSES.md:6) describe candidates under `assets/uct/golden/...`, including “committed candidates.” That directory is absent. Identify `assets/...` as the proposed future source location and the supplied work-data `render-01/` directory as the actual current location. This requires documentation changes only.

Each row below matches the generator’s authored literal, manifest and provenance. Raw SHA-256 values were independently recomputed and match all three recorded pins; all eight exact UTF-8 text hashes also match provenance.

| ID | Verdict | Exact source text | Width × height | Bytes | Raw SHA-256 |
|---|---|---|---:|---:|---|
| words-regular | OK | `OkuFlow` | 170 × 48 | 8,160 | `845e08157d5bd1a5d73ae9fb535314564700a64aec884e8cac10c6d9953f25bf` |
| phrase-bold | OK | `Please read this line` | 386 × 48 | 18,528 | `25cef458654478d98ce599040fafdb3aa28334bd6bb7cf0fdfc5ae8455301314` |
| date-regular | OK | `2026-10-23` | 200 × 48 | 9,600 | `85b4ab1d0311dfc64a9da23d9d00425705b8ff2af49af88c74c530843a384204` |
| decimal-bold | OK | `+12.50 -0.75` | 245 × 48 | 11,760 | `7fae70370384a4f5f1472e4b48ee8621ea48474bbc948f3cf849c9bcc7cbfafb` |
| o-zero-regular | OK | `O0 OO 00` | 191 × 48 | 9,168 | `efa374434bc1bb53274870bfdce5c21c85dd762433966e8102c6b962fb203068` |
| i-l-one-inverse | OK | `Il1 I l 1` | 155 × 48 | 7,440 | `3d27de8c915fe355195e9fa068becb75ee20aeb28e1ca1eddb4de02f8fd5ebee` |
| negation-bold | OK | `Do not enter` | 257 × 48 | 12,336 | `df8f31f8c127e18fd8cad2cb8a65ed3547ec157ddedb670c6ac3cabc2901d27a` |
| accents-inverse | OK | `café naïve` | 193 × 48 | 9,264 | `66bac6b26ac8ccd8db4c89d446503eb77f8c187c3bf9d84a981f85e0fd903783` |

Total raw size is **86,256 bytes**. Every length equals `width × 48`, and every width is within 1–1024. Inspection of the existing contact sheet showed upright, unclipped crops with useful widths, the stated polarities, signs, decimal points, negation and accents. Source codepoints confirm capital `I` U+0049, lowercase `l` U+006C, digit `1` U+0031, and precomposed é U+00E9 / ï U+00EF. Required tags describe actual source coverage.

The static contract matches the three requested files read through `git show` at exact commit `faa7fbfe7b87983802c881411a856328bc873dfd`:

- Exactly seven manifest root fields and five fields per sample.
- **25/25 `ModelIdentity` keys, types and values match**, including source/graph/config/dictionary pins.
- Profile minimum `[1,3,48,1024]`, optimum/maximum `[8,3,48,1024]`; capacity 8; fixed1024; FLOAT DEVICE LINEAR input/output; 18,710 classes; CTC ABI; TF32 default, optimization 2, workspace 256 MiB, auxiliary streams 0.
- Policy, encoding and preprocessing match. Stored data is useful-width GRAY8; future runtime padding is **normalized zero**, with no appended raw white padding.

Independently recomputed artifact hashes match:

- Generator: `b4b8093159396aaabbccd8f171fc39c85585f01b743579c9da8add52b290aeca`
- Manifest: `26432369ed6831431737072f857620b19c81fc9acd8c21579d50ee7bc0e9ec33`
- Provenance: `981ca01ca6d00dccf38b6987b2a867995f9c61e26d19c966a57a7e2ff8a1f4af`
- Contact sheet: `0192f4ef4b17f6153ca9cfe4412ed57138492b55848f55fb24bafc0d87ad59cd`
- Proposal: `767b70ed74d82780e640f19bdc4b6647eb55c1f6964ccfab274d3035c8c081d5`

Both installed font hashes and both installed notice hashes match their generator/provenance pins. Notice contents support the stated Bitstream Vera/Arev/DejaVu and Pillow MIT-CMU facts. Provenance records Pillow 11.1.0, FreeType 2.13.3, Python 3.11.13 and zlib 1.2.13 with the stated drawing parameters. Invocation/completion records report one successful process, zero reruns and empty stderr. No font/tool binaries, copied third-party implementation or source artwork are added; no new clearance claim was found. JSON artifacts use LF with final newlines, and the proposed attributes preserve exact bytes.

This was read-only source, JSON, hashing and existing-image inspection. I made no edits and ran no Python, render, tests, model or GPU work. **ProductionCorpusCatalog remains empty/unapproved.** Parent visual review, actual runtime quality, N1 plus full N8 exact-text attestation, and explicit separate catalog approval remain outstanding; this review supplies no golden pass, ready receipt or performance acceptance.

