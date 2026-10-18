# GPU selected-video playback (development)

CUDA-enabled Windows builds prefer the native GPU path for saved files and the
same-session Original archive. The owner supplies the presenter's exact adapter
LUID before Open. CPU-only builds, unavailable adapters and unsupported native
GPU output use the existing bounded CPU reader. No new media framework, decoder
DLL, model download or codec extension is bundled.

## Pixels and ownership

The one playback worker attaches an `IMFDXGIDeviceManager` to the Media Foundation
Source Reader, enables hardware transforms/DXVA, and negotiates a supported
uncompressed output. It accepts actual DXGI samples, checks their device,
subresource, aperture, pixel aspect, orientation and color metadata, then uses a
D3D11 video processor to produce an owned, square-pixel, NT-shareable BGRA texture.
A driver that refuses a processor view of a shared target can use one private
processor target followed by a same-context GPU copy. Both paths complete their
query before publishing; a published frame never pins a reusable MF sample.
Before submitting a conversion, the pending owner captures the reader, sample,
buffer, DXGI resource, processor views, destination and query. A failed Blt that
never ended its query is never polled as if it completed. Unknown completion
quarantines that entire owner; later decoder calls cannot reuse it. CPU fallback
gets a fresh cursor over the same held file backing, so an old native reader's
prefetch cannot race the replacement or reopen a changed pathname.

`VideoPlaybackFrame` carries either immutable CPU `bgra` or an owned `gpu` image.
Reticketed fine scrubs share the original allocation. The global decoder texture
pool admits at most eight allocations and 256 MiB of logical BGRA payload,
including the optional private conversion target and resources awaiting final
native release. Leased frames are never overwritten. Final texture release is
queued to one bounded worker so the last UI-side lease cannot run a driver's
texture destructor. Driver padding and decoder reference surfaces are additional.

The decoder does not read video pixels to the CPU during normal GPU playback.
Native calls, metadata and command scheduling still run on the CPU. A real native
DXGI surface is required; enabling a hardware-transform preference alone is not
treated as proof that a software decoder became GPU-resident.

## Processing and presentation

`MediaGpuScene` owns a separate shared D3D12 output, CUDA surface and private
shared fence. Initialization and retirement run on workers. Its UI-facing methods
poll readiness and submit work; they do not join those workers or wait for OCR.
The exact decoded image lease stays alive through CUDA's source-copy event.
Source imports are prepared on one bounded worker and cached across up to eight
rotating decoder slots. The UI adopts ready imports without native texture
import/destruction calls. First use can return Pending; steady-state slot reuse
is a cache hit.

Video receives the same `BuildProcessingSettings` mapping as the camera: blur,
spatial and smart sharpening, Text Clarity and its refinements, auto contrast,
stabilization/Extra Stable, keystone, temporal smoothing, reading colors and
Super Resolution. Legacy in-CUDA zoom remains off because presentation owns
zoom. Each media scene has independent history and a bounded spatial/neural
cache. Seek/source/request discontinuities and nonmonotonic PTS reset history
and revoke neural tickets; ordinary forward playback preserves history. A
same-frame view/appearance edit replays saved geometry and the pre-frame effect
histories, so polling a paused image does not repeatedly blend/register it.
One in-flight processing pass bounds rapid ROI updates; the next pass uses the
newest selected request. Explicit text/SR edits revoke pending enhancement
independently of another decoder frame; an SR toggle also clears its retry latch.

The existing private Maxine producer loads models and performs inference off
UI. Paused playback polls its publication without another decoder frame.
Native settings and actual effect/readiness state still determine capability;
a CPU-only rendering fallback never claims these GPU effects. A CPU-decoded
BGRA source still uploads once through the owned pinned ring and uses the same
CUDA processing when that adapter is healthy; decoder fallback alone does not
disable effects. Original recording admission stays clean and independent of
all playback appearance.

Pan/zoom/Fit/Fill reuse the processed scene. Spatial/neural ROI updates replay
that same frame without advancing history, while presentation geometry can
continue independently of inference. A successful receipt carries source/selection
epoch, exact frame,
appearance, view transform, viewport and focus marker. Busy draws preserve the
previous receipt. Source or geometry changes revoke earlier OCR/capture authority.

The private fence orders CUDA writes and each graphics read, including snapshots.
A graphics value is recorded only after its queue signal was actually submitted.
It never reserves or commits a camera CUDA value on the global presenter timeline.
The ordinary global graphics signals still pace command allocators and readbacks.
A queued private-fence wait retains its fence even if a later signal fails.
Completed source texture/fence references retire on one bounded worker after
that ordinary graphics timeline proves their reads complete; opening many files
does not accumulate every prior scene until presenter destruction.
Unknown completion retains the affected resource closure instead of freeing live
GPU memory or allowing another writer to reuse it.

## UCT and explicit image actions

The CUDA scene captures owned pre-zoom device analysis with the exact epoch,
frame sequence and appearance. Paused media uses the shared immutable controller;
optional corroboration has its own scene and genuine distinct frame. Advancing
media uses the existing paced probe/OCR workers. A missing analysis lease is normal
backpressure: `OnAdvancingMediaPresentedWithoutAnalysis` advances only the real
displayed PTS and geometry. It cannot restamp an old image or renew validation.
Prior glyphs survive brief gaps only while their actual media and wall validation
remain fresh (350 ms by default). Seek/source/backward transitions revoke them.
Consumed advancing device leases are released independently of receipt identity.

GPU-capable OCR consumes device inputs. CPU OCR fallback may download its owned
input on the recognition worker. Independent decimated motion validation also
uses bounded readback; neither transfer blocks presentation. A selected paused
image intentionally retains its analysis input for one-shot recognition.
Analysis follows stabilization/keystone geometry and precedes appearance. If
an active temporal/hold composite or neural presentation cannot prove a
single-frame text mapping, the media correspondence gate keeps the user's UCT
preference but revokes glyphs, pane text, jobs and positioned UIA exposure.
Returning to a provable view requires fresh, normally validated evidence.

Read/Explain, Assistant attachments, authorized observations and annotation
snapshots request asynchronous copies of the successfully selected viewport.
They retain exact request identity through cancellation and never interpret a
full-source hold copy as a viewport observation. Save Frame first owns a GPU
clone of the clicked viewport, focus marker and admitted UCT/ink. Playback,
seeking and source changes cannot overwrite it. The asynchronous ring retains
that clone until copy completion; after a resize drops the ring receipt, the
same clone is copied again. Late timed-out save copies are discarded before
other image consumers. Only explicit export/analysis needs full viewport pixels.
The media observation's frame sequence and document PTS come from the exact
submitted candidate, including the offset of later recording parts, rather than
the previous `doc.presented` state. Unmapped live holds retain unknown PTS. Read/Explain and
attachments pin the currently displayed frame in the click's UI turn. A new
document never inherits an earlier document's display receipt. Annotation copy
expiry also runs while media is paused, so a dropped resize readback cannot
keep later saves busy indefinitely.

## Format support and boundaries

Negotiation is based on installed decoder/output and video-processor capability,
not a small codec-name allowlist. Proven coded precision (bounded container
configuration metadata) chooses an appropriate order; otherwise a bounded
merit-ordered native decoder probe supplies its offered output preference.
Accepting a tolerated partial type is not proof that it can stream: the VP9
regression caught an 8-bit source accepting P010 and then blocking its first
native read. Decoder preference avoids that unnecessary upconversion while
preserving actual high-precision HEVC/AV1 outputs. Eligible output includes NV12, P010/P016,
YUY2, AYUV, Y410/Y416 and BGRA/RGB32. Color/transfer conversion must be advertised
by the device. Unsupported range, interlace, fractional aperture, color conversion,
missing DXGI output, adapter or CUDA-import failure uses CPU compatibility when
that system converter supports the file. HDR tone mapping and every codec/profile
on every GPU are not certified by negotiation or by a successful SDR fixture.
The existing admitted geometry/working-set limits still apply; this does not
promise arbitrary 8K or unlimited externally retained frames.
The accepted native output precision is exposed as
`gpuDecodedBitsPerComponent`; a 10-bit source is tested against an actual
10-bit-or-higher decoder output before conversion to the current 8-bit display
scene. Explicit Unknown metadata does not suppress valid source metadata.
Temporary texture credit, stale requests, revoked archive bytes and ordinary
seek deadlines are distinct from GPU incompatibility and do not themselves
permanently downgrade a healthy GPU decoder.

The CPU decoded-frame allowance (96-256 MiB), decoder GPU texture allowance,
processed CUDA scene(s), CUDA scratch, UCT analysis/probe/model buffers, viewport,
recorder Original pool and one explicit save clone are distinct budgets. Neither
96 MiB nor 256 MiB is a total application-RAM/VRAM limit. Pressure drops optional
lookahead/corroboration and preserves bounded ownership rather than growing a
history buffer. Normal unrecorded camera use still has no playback history.

## Evidence scope

Window-free native cases exercise AVC/AV1 active and finalized playback,
UHD/DCI seeks, immutable leases and CPU fallback. Synthetic NV12/P010 checks
inspect actual converted pixels, bounded leases and the private-target path.
`media_gpu_scene_cuda` runs the production CUDA scene on exact-adapter private
textures and checks rotation, appearance, device UCT identity and fence/lifetime
behavior. `video_gpu_snapshot` uses production presenter shaders, snapshot pool
and readback commands without an HWND, checking pixels, overlays, private/global
fence separation and resize-style retry.

Optional absolute `OKUFLOW_TEST_VIDEO_GPU_FILES` CMake inputs register additional
`video_gpu_format_*` cases with exact fixture files in `REQUIRED_FILES`. Locally
generated HEVC Main10, AV1 10-bit and VP9 files require actual resident output,
seek and EOF. FFmpeg is used only to create these private synthetic test inputs;
it is not an application or ordinary test-suite dependency.

These checks do not measure native playback FPS, pan-to-photon latency, audio
hardware sync, HDR display quality, Narrator/NVDA, mixed-monitor DPI, long soak or
installer acceptance. GUI remains separate and serial on an explicitly free
desktop. See [validation record](accessible_video_implementation.md).
