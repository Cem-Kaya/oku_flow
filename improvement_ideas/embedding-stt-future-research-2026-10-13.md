# Future research: speech recognition from EmbeddingGemma 2

**Reviewed 2026-10-13. Status: research only; no implementation selected.**

The model and its audio support are real. Its shared audio/text embedding space
does not establish a working speech recognizer. The reported one-hour transcription
in roughly 13 seconds on a base M3 MacBook Air remains unverified: this review
found no public author implementation, checkpoint, transcript, or reproducible ASR
benchmark for that claim. Preserve the existing free, server-free Chrome/WebGPU
transcription workflow; this note authorizes no model or product changes.

## Verified model identity

| Model | Availability | Boundary |
| --- | --- | --- |
| Gemini Embedding 2 | Google announced its multimodal API public preview on March 10, 2026. Current API docs use `gemini-embedding-2`. | Hosted `embedContent` API; reviewed sources provide no downloadable weights. Cannot substitute for a local browser model. |
| EmbeddingGemma 2 | `google/embeddinggemma-2`, released October 6, 2026 with open weights under Apache 2.0. | Multimodal embedding model; no advertised ASR checkpoint. |
| EmbeddingGemma 1 | Earlier 308M Gemma 3 based text embedding model. | Older browser examples do not establish audio support for this model. |

Sources: [Gemini launch](https://blog.google/innovation-and-ai/models-and-research/gemini-models/gemini-embedding-2/),
[current Gemini API](https://ai.google.dev/gemini-api/docs/embeddings),
[EmbeddingGemma 2 launch](https://blog.google/innovation-and-ai/technology/developers-tools/embeddinggemma-2/),
[Google overview and previous version](https://ai.google.dev/gemma/docs/embeddinggemma).

EmbeddingGemma 2 has approximately 740M parameters: 270M text, optional 300M audio
and 170M vision components. Audio/text can omit vision and use approximately 570M.
The shared input budget is 8,192 tokens; audio uses 25 tokens/second, allowing
approximately 327 seconds alone. Input audio should be mono at 16 kHz. Normal
output is a 768-dimensional semantic vector, with supported shorter dimensions
of 512, 256 and 128. The card evaluates embedding benchmarks including MSEB and
MAEB, rather than transcription WER/CER or word timing. Multilingual embedding
capability is not a measured multilingual ASR guarantee.
[Google model card](https://huggingface.co/google/embeddinggemma-2)

Publisher metadata identified the checkpoint as
`google/embeddinggemma-2@914f7f89142e33e77833254d9c9b90c3cef7303b`.
[Publisher metadata](https://huggingface.co/api/models/google/embeddinggemma-2)

Apache 2.0 is a promising starting point. Later work must review exact derivative
weights, decoding code, tokenizer, training data and notices together. This note
adds no redistributed dependency or model. Another Gemma-family license must not
be assumed to apply; derivatives are not cleared for GPL/commercial distribution
merely by recording this research.

## Embeddings are not automatically transcripts

The architecture projects audio into a bidirectional text encoder. The Python
interface exposes per-token `last_hidden_state`; sentence embeddings use
mask-aware mean pooling and L2 normalization. Vocabulary and audio placeholder
tokens identify inputs, not decoded output words.
[Transformers architecture and pooling documentation](https://huggingface.co/docs/transformers/v5.19.0/en/model_doc/embedding_gemma2)

Inference: audio/text similarity can rank known candidate sentences. It provides
no general inverse mapping from a pooled vector to exact previously unknown
words. Paraphrases may be semantically near while differing in names, numbers,
negation or wording. Mean pooling does not expose word-by-word chronology.

Plausible future approaches require additional work:

- **Candidate retrieval:** match short audio against pre-embedded sentences or
  labels. Useful for fixed vocabulary, search or routing, but not unrestricted
  lecture transcription.
- **Sequence recognizer:** train a CTC, transducer or sequence decoder on
  time-resolved audio features/token representations. Determine which features
  retain phonetic information, whether the encoder can be frozen, and how
  bidirectional context affects alignment. Training and decoder weights add cost
  and dependencies.
- **Embedding-conditioned generation/inversion:** supply or train a text decoder.
  Measure lexical recovery, hallucinations and full output latency. A fluent
  reconstruction is not evidence of faithful ASR.
- **Search alongside ASR:** embed recording chunks for passage retrieval, retaining
  a tested recognizer for readable transcripts. This matches Google's documented
  use cases most closely, but remains future work here.

The 25 audio tokens/second are representation positions, not word timestamps.
Recognition needs an explicit alignment method. Chunk start/end times locate
passages coarsely but do not establish word onsets. One hour exceeds the input
budget and needs chunking, boundary handling and output assembly.

## Shahir's speed claim: unverified

The user supplied a post attributed to Shahir, `@TslShahir`, describing conversion
to STT and about 288 times real-time processing on a base M3 MacBook Air. Searches
for the handle, model, transcription, M3 and speed figures did not locate public
reproduction materials. No exact status URL was available to pin the quoted claim.

The author's [own portfolio](https://shahir.thesmartlanguage.com/) links the handle
to GitHub user `shershah1024`. Its [public repository listing](https://api.github.com/users/shershah1024/repos?per_page=100)
on the review date contained no EmbeddingGemma STT project. The public
[qwen3-asr-llamacpp project](https://github.com/shershah1024/qwen3-asr-llamacpp)
is a separate recognizer, not evidence for this conversion. Private/unindexed
code could exist; this search does not prove the experiment impossible or fabricated.

Exactly 3,600 seconds / 13 seconds is approximately **277 times real time**
(RTF 0.00361); 288 times would mean 12.5 seconds for exactly one hour. Approximate
wording can explain the difference. Publish rates only with exact duration and
wall time.

Unknowns include decoder/training method, language, audio sample, WER/CER,
punctuation, timestamps, silence filtering, batching, precision, GPU synchronization
and inclusion of file decoding, resampling, loading, features and complete text
output in timing. Rapidly embedding an hour and rapidly transcribing it are
different claims.

## Chrome/WebGPU feasibility

A conversion exists:
`onnx-community/embeddinggemma-2-ONNX@daa72c51243991dfcaf9f9137d2c573d8f7790c0`.
Its card gives Transformers.js WebGPU audio-input examples returning
`sentence_embedding`. Listed q4 text/audio files are 175 MB / 189 MB; q4 text plus
q8 audio totals about 515 MB before tokenizer, activations, runtime and browser
overhead. The converter warns audio is affected most by 4-bit quantization.
These are publisher figures, not measured OkuFlow memory or ASR accuracy.
[Conversion author's model card](https://huggingface.co/onnx-community/embeddinggemma-2-ONNX)

The first-party Transformers.js repository has an
[EmbeddingGemma 2 implementation](https://github.com/huggingface/transformers.js/blob/511bb61a7c46b73e33f1af3fb308fbd1afe98c02/packages/transformers/src/models/embedding_gemma2/modeling_embedding_gemma2.js)
returning per-token states and pooled embeddings, plus
[processor code](https://github.com/huggingface/transformers.js/blob/511bb61a7c46b73e33f1af3fb308fbd1afe98c02/packages/transformers/src/models/embedding_gemma2/processing_embedding_gemma2.js).
Its inspected [package](https://github.com/huggingface/transformers.js/blob/511bb61a7c46b73e33f1af3fb308fbd1afe98c02/packages/transformers/package.json)
declares 4.3.1. This establishes an available browser embedding path, not an STT
decoder. Later experiments must pin and verify the published package and artifacts.

Google recommends BF16/FP32 and warns against original-checkpoint FP16 activation
overflow. The converter advertises FP16 and near-FP32 cosine similarity. These
statements concern different artifacts: review conversion numerical handling and
validate finite outputs/task quality rather than blindly casting original weights.
Quantization needs ASR-specific evaluation if a decoder is added.
[Original precision warning](https://huggingface.co/google/embeddinggemma-2#4-numerical-precision)

ONNX Runtime Web supports WebGPU and documents CPU/GPU copies and optional IO
binding. Static-shape graph capture requires all kernels on WebGPU. A future
decoder needs compatible operators, preprocessing, shape handling and any
cache/alignment operations; conversion success alone is insufficient.
[ONNX Runtime WebGPU documentation](https://onnxruntime.ai/docs/tutorials/web/ep-webgpu.html)

MLX benefits from Apple Silicon's unified memory.
[Apple MLX memory documentation](https://github.com/ml-explore/mlx/blob/main/docs/src/usage/unified_memory.rst)
Inference: an M3 native Metal/MLX/CoreML result does not establish Windows Chrome
performance. Dispatch, adapter limits, shader compilation, precision, copies and
additional decoding can change results. The post's exact backend remains unknown.

## Comparison target: Whisper large-v3-turbo

Whisper large-v3-turbo is an approximately 809M-parameter multilingual ASR
encoder-decoder, reducing large-v3's decoder from 32 layers to four. Its publisher
supplies transcription, segment/word timestamp examples and long-form strategies
around 30-second windows. It still needs evaluation for errors, hallucinations and
chosen languages; the publisher warns performance varies.
[Whisper publisher card](https://huggingface.co/openai/whisper-large-v3-turbo)

| Requirement | Whisper evidence | EmbeddingGemma 2 evidence today |
| --- | --- | --- |
| Unknown multilingual speech to text | ASR decoder and examples | Audio embeddings; claimed conversion's decoder unverified |
| Word fidelity | ASR metrics; still fallible | Retrieval scores cannot replace WER/CER |
| Segment/word times | Documented timestamp workflows | Representation positions; alignment unverified |
| One-hour lecture | Documented long-form strategies | Approximately 5.5-minute embedding budget; transcript assembly unverified |
| Windows Chrome speed | Measure actual browser profile | Native M3 claim does not answer this |

Parameter count alone is not a speed comparison: graphs and output tasks differ.
No speedup or quality parity with Whisper is established here.

## Later reproduction and decision gates

Undertake experiments only after a separate user decision, outside production.
Keep downloaded models/data/caches in local scratch space outside Drive/Git.

1. Obtain original post URL, author code commit, checkpoint, license,
   decoder/training description and command. Establish whether it generates
   arbitrary transcripts, selects candidates or only computes vectors.
2. Reproduce M3 results if available. Synchronize GPU completion; report exact
   duration, repeated trials, cold/warm and full pipeline times with separate
   stages. Include auxiliary models/services used to generate text.
3. Use held-out licensed English, Turkish and German audio/reference transcripts,
   unseen speakers/topics, names, numbers, technical terms, code switching,
   silence/noise and 30/60/120-minute lectures. Test repetitions and boundaries;
   separate any training material from evaluation audio.
4. Compare current browser and pinned Whisper large-v3-turbo profiles on identical
   devices/audio. Report WER/CER per language, deletions/insertions, silence
   hallucinations, punctuation, name/number fidelity, missing/repeated boundary
   text and timestamp error. Measure retrieval separately from ASR accuracy.
5. Validate Windows Chrome on integrated/discrete GPUs; record adapter/driver,
   Chrome/runtime versions, precision, download size, cold startup, peak CPU/GPU
   memory, throughput, responsiveness, cancellation, device loss and long-session
   behavior. Prove local operation without uploading recordings or silent services.
6. Require useful free-form multilingual accuracy, truthful times, browser resource
   fitness, reviewed licenses and a reproducible end-to-end gain before proposing
   replacement. Otherwise retain embedding/search research only.

**Conclusion:** direct local audio search has published artifacts. Embedding-derived
STT is plausible research with an unverified decoder and benchmark; it is not an
evidence-based replacement today. This review downloaded no models, ran no GPU
benchmarks, and changed no app, Android, UI, site, transcription configuration or
runtime dependency. Documentation-only changes require no build/test matrix.
