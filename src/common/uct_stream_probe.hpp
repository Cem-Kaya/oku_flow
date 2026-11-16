#pragma once

// Round 8, step 1 (docs/uct_okuread_port.md): does a second inference of one
// TensorRT engine, on its own execution context and CUDA stream, finish
// sooner beside the first than after it?
//
// Test builds only. The probe takes the engines of a ready GPU reader and
// creates its own execution contexts (each with its own activation memory),
// input and output buffers and streams; it never uses the reader's contexts,
// stream or buffers. For each engine and input shape it times one set of
// inferences on one context and one stream (the reader's arrangement today)
// and the same set fed alternately to two and more contexts from one host
// thread, once with plain enqueues and once with each context's enqueue
// replayed as a CUDA graph, and checks every output against the
// single-stream output byte for byte.

#include <QJsonObject>

namespace okuflow::uct
{
class Recognizer;
struct StreamProbeRequest {
    int recognizerInferences{64}; // at batch eight on the 1024 canvas
    int detectorInferences{32};   // per detector shape of a 1920x1080 read
    int repeats{7};               // timed sets per configuration, after one untimed set
    int maximumStreams{3};        // contexts/streams compared: 1 to this
    int distinctInputs{4};        // inputs cycle through this many deterministic patterns
};
#if defined(OKUFLOW_UCT_GRAPH_FAULT_HOOKS)
// `recognizer` must be a ready TensorRT reader that has completed at least
// one read (so its State is adopted), with graph replay off and no read in
// flight. Throws on any CUDA or TensorRT failure.
QJsonObject TensorRtStreamProbeForTest(Recognizer& recognizer, const StreamProbeRequest& request);
#endif
} // namespace okuflow::uct
