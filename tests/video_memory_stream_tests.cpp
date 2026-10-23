#ifdef _WIN32

#include "okuflow/common/video_memory_stream.hpp"
#include "okuflow/common/media_writer.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace okuflow {
// Exercise the actual first-party IStream used by MF, including writable
// clones not directly exposed by IMFByteStream's public surface.
struct VideoMemoryStreamTestAccess {
    static HRESULT Cursor(VideoMemoryStream& output, IStream** cursor) {
        return output.OpenWriteCursor(cursor);
    }
    static std::weak_ptr<const VideoMemoryBacking> Backing(const VideoMemorySource& source) {
        return source.backing_;
    }
    static HRESULT Begin(VideoMemoryStream& output) { return output.BeginRecording(); }
    static HRESULT Retry(VideoMemoryStream& output) { return output.ResetForRetry(); }
    static void Finish(VideoMemoryStream& output, bool complete) {
        output.FinishRecording(complete);
    }
};

struct VideoRecorderTestAccess {
    static void PrintAdmission(VideoRecorder& recorder,int frame) {
        MF_SINK_WRITER_STATISTICS stats{};stats.cb=sizeof(stats);
        const auto hr=recorder.sinkWriter_->GetStatistics(recorder.streamIndex_,&stats);
        std::cerr<<"RAM admission frame="<<frame<<" hr="<<hr<<" received="<<stats.qwNumSamplesReceived
            <<" encoded="<<stats.qwNumSamplesEncoded<<" queuedBytes="<<stats.dwByteCountQueued
            <<" submitted="<<recorder.videoSamplesWritten_<<" pending="<<bool(recorder.pendingSample_)<<'\n';
        Microsoft::WRL::ComPtr<ICodecAPI> codec;
        const auto service=recorder.sinkWriter_->GetServiceForStream(recorder.streamIndex_,GUID_NULL,IID_PPV_ARGS(&codec));
        VARIANT value{};const auto latency=codec?codec->GetValue(&CODECAPI_AVLowLatencyMode,&value):service;
        std::cerr<<"RAM codec service="<<service<<" lowLatencyHr="<<latency<<" type="<<value.vt<<" value="<<value.lVal<<'\n';
        VariantClear(&value);
        Microsoft::WRL::ComPtr<IMFSinkWriterEx> extended;
        if(SUCCEEDED(recorder.sinkWriter_.As(&extended)))for(DWORD i=0;i<8;++i) {
            Microsoft::WRL::ComPtr<IMFTransform> transform;GUID category{};
            if(FAILED(extended->GetTransformForStream(recorder.streamIndex_,i,&category,&transform)))break;
            Microsoft::WRL::ComPtr<IMFAttributes> attrs;GUID clsid{};wchar_t name[128]{},id[40]{};
            if(SUCCEEDED(transform->GetAttributes(&attrs))) {
                attrs->GetGUID(MFT_TRANSFORM_CLSID_Attribute,&clsid);attrs->GetString(MFT_FRIENDLY_NAME_Attribute,name,128,nullptr);
            }
            StringFromGUID2(clsid,id,40);std::wcerr<<L"RAM transform["<<i<<L"] clsid="<<id<<L" name="<<name<<L'\n';
        }
    }
    static void ObserveSink(VideoRecorder& recorder,
                           std::function<void(bool, IMFByteStream*)> observer) {
        recorder.sinkInitializationObserver_ = std::move(observer);
    }
};
} // namespace okuflow

namespace {
using Microsoft::WRL::ComPtr;
using okuflow::VideoMemoryStream;
using Access = okuflow::VideoMemoryStreamTestAccess;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Ok(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }
LARGE_INTEGER Offset(std::int64_t offset) {
    LARGE_INTEGER value{}; value.QuadPart = offset; return value;
}
ULARGE_INTEGER Size(std::uint64_t size) {
    ULARGE_INTEGER value{}; value.QuadPart = size; return value;
}

void BoundsAndCursors() {
    VideoMemoryStream output(16);
    Require(output.ByteSize() == 0 && output.Capacity() == 0, "storage is lazy");
    Require(output.OpenWriteStream(nullptr) == E_POINTER, "null MF output");
    ComPtr<IStream> cursor;
    Ok(Access::Cursor(output, &cursor), "open production IStream");
    const std::array<BYTE, 6> input{1, 2, 3, 4, 5, 6};
    ULONG written = 99;
    Ok(cursor->Write(input.data(), static_cast<ULONG>(input.size()), &written), "write bytes");
    Require(written == input.size() && output.ByteSize() == input.size() &&
        output.Capacity() >= input.size() && output.Capacity() <= 16, "actual charged capacity");
    const auto capacity = output.Capacity();
    Require(!output.Seal(), "writable cursor prevents Seal");
    Require(FAILED(cursor->Write(nullptr, 1, &written)) && written == 0, "null write refusal");
    Ok(cursor->Write(nullptr, 0, &written), "zero write");
    ULARGE_INTEGER position{};
    Require(FAILED(cursor->Seek(Offset(17), STREAM_SEEK_SET, &position)), "seek beyond cap");
    Require(FAILED(cursor->Seek(Offset(-7), STREAM_SEEK_CUR, &position)), "negative seek");
    Require(FAILED(cursor->Seek(Offset(std::numeric_limits<std::int64_t>::min()),
        STREAM_SEEK_CUR, nullptr)), "INT64_MIN seek");
    Require(FAILED(cursor->Seek(Offset(std::numeric_limits<std::int64_t>::max()),
        STREAM_SEEK_CUR, nullptr)), "positive seek overflow");
    Require(FAILED(cursor->Seek(Offset(0), 99, nullptr)), "invalid seek origin");
    Ok(cursor->Seek(Offset(0), STREAM_SEEK_CUR, &position), "query cursor");
    Require(position.QuadPart == input.size(), "failed seeks preserve cursor");
    Ok(cursor->Seek(Offset(15), STREAM_SEEK_SET, nullptr), "seek hole within cap");
    written = 99;
    Require(cursor->Write(input.data(), 2, &written) == STG_E_MEDIUMFULL && written == 0,
        "write refuses before partial mutation");
    Require(output.ByteSize() == input.size() && output.Capacity() == capacity,
        "failed growth leaves bytes and capacity intact");
    Ok(cursor->Write(input.data(), 1, &written), "write last byte");
    Require(output.ByteSize() == 16 && output.Capacity() <= 16, "cap boundary");
    Require(cursor->SetSize(Size(std::numeric_limits<std::uint64_t>::max())) == STG_E_MEDIUMFULL,
        "SetSize unsigned overflow refusal");
    Require(cursor->SetSize(Size(17)) == STG_E_MEDIUMFULL, "SetSize above cap");
    Ok(cursor->SetSize(Size(3)), "truncate");
    Ok(cursor->SetSize(Size(8)), "extend zeroes");
    Require(output.Capacity() == capacity, "shrink retains charged allocation");
    Ok(cursor->Seek(Offset(0), STREAM_SEEK_SET, nullptr), "rewind writer");
    std::array<BYTE, 16> bytes{};
    ULONG read{};
    Require(cursor->Read(bytes.data(), 16, &read) == S_FALSE && read == 8, "short read at EOF");
    Require(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 &&
        std::all_of(bytes.begin() + 3, bytes.begin() + 8, [](BYTE x) { return x == 0; }),
        "extended bytes are zero, never old truncated bytes");
    Ok(cursor->Seek(Offset(2), STREAM_SEEK_SET, nullptr), "position for clone");
    ComPtr<IStream> copy;
    Ok(cursor->Clone(&copy), "clone production writable cursor");
    Ok(copy->Read(bytes.data(), 1, &read), "clone read");
    Require(bytes[0] == 3, "clone starts at same position");
    Ok(cursor->Seek(Offset(0), STREAM_SEEK_CUR, &position), "original position");
    Require(position.QuadPart == 2, "clone cursor is independent");
    STATSTG stat{};
    Ok(copy->Stat(&stat, STATFLAG_DEFAULT), "memory stat");
    Require(!stat.pwcsName && stat.type == STGTY_STREAM && stat.cbSize.QuadPart == 8,
        "stat has no filesystem identity");
    cursor.Reset();
    Require(!output.Seal(), "writable clone alone prevents Seal");
    copy.Reset();
    const auto source = output.Seal();
    Require(source && source->ByteSize() == 8 && source->Capacity() == 8 &&
        output.Capacity() == source->Capacity(), "seal compacts actual retained allocation");
    Require(output.Seal().get() == source.get(), "Seal preserves opaque source identity");
    Require(FAILED(output.OpenWriteStream(nullptr)), "sealed null stream refusal");
    Require(Access::Cursor(output, &cursor) == STG_E_ACCESSDENIED && !cursor,
        "sealed output cannot acquire stale writers");
    Require(source->OpenReadStream(nullptr) == E_POINTER, "null read output");

    VideoMemoryStream empty(0);
    Ok(Access::Cursor(empty, &cursor), "zero-cap cursor");
    Require(cursor->Write(input.data(), 1, &written) == STG_E_MEDIUMFULL, "zero-cap write");
    Require(cursor->SetSize(Size(1)) == STG_E_MEDIUMFULL, "zero-cap resize");
    Require(FAILED(cursor->Seek(Offset(1), STREAM_SEEK_SET, nullptr)), "zero-cap seek");
    Ok(cursor->SetSize(Size(0)), "zero-cap empty resize");
    Require(empty.Capacity() == 0 && empty.ByteSize() == 0, "zero cap never allocates bytes");
}

void AdapterAndEvictedLeases() {
    auto output = std::make_shared<VideoMemoryStream>(64);
    ComPtr<IMFByteStream> writer;
    Ok(output->OpenWriteStream(&writer), "open real MF write adapter");
    constexpr std::array<BYTE, 8> input{11, 22, 33, 44, 55, 66, 77, 88};
    ULONG written{};
    Ok(writer->Write(input.data(), static_cast<ULONG>(input.size()), &written), "adapter write");
    Require(written == input.size(), "adapter writes complete bytes");
    Require(FAILED(writer->SetLength(65)), "adapter SetLength cap");
    Require(FAILED(writer->SetCurrentPosition(std::numeric_limits<QWORD>::max())),
        "adapter seek overflow");
    QWORD rejectedPosition{};Ok(writer->GetCurrentPosition(&rejectedPosition),"query adapter after failed seek");
    Require(rejectedPosition==input.size(),"failed adapter seek preserves exact cursor");
    Require(FAILED(writer->Seek(msoCurrent,std::numeric_limits<LONGLONG>::min(),0,nullptr)),"adapter signed seek underflow");
    Require(FAILED(writer->Seek(msoCurrent,std::numeric_limits<LONGLONG>::max(),0,nullptr)),"adapter signed seek overflow");
    Ok(writer->SetCurrentPosition(0), "adapter rewind");
    Ok(writer->Flush(), "adapter flush");
    Require(!output->Seal(), "live MF adapter prevents Seal");
    Ok(writer->Close(), "close adapter");
    Require(!output->Seal(), "Close without Release still prevents Seal");
    writer.Reset();
    auto source = output->Seal();
    Require(source != nullptr, "released adapter permits Seal");
    const auto capacity = source->Capacity();
    Require(capacity == input.size(), "sealed MF payload charges exact compact capacity");
    std::weak_ptr<const okuflow::VideoMemoryBacking> lease = Access::Backing(*source);
    ComPtr<IMFByteStream> first, second;
    okuflow::VideoReadStreamClone clone;
    Ok(source->OpenReadStream(&first, &clone), "read source with clone factory");
    Ok(clone(&second), "independent MF read clone");
    std::array<BYTE, 8> bytes{};
    ULONG read{};
    Ok(first->Read(bytes.data(), 3, &read), "first cursor prefix");
    Require(read == 3 && bytes[0] == input[0], "first read bytes");
    QWORD position{};
    Ok(second->GetCurrentPosition(&position), "clone initial position");
    Require(position == 0, "factory clone begins at zero");
    DWORD capabilities{};
    Ok(second->GetCapabilities(&capabilities), "read capabilities");
    Require((capabilities & MFBYTESTREAM_IS_READABLE) &&
        !(capabilities & MFBYTESTREAM_IS_WRITABLE), "sealed adapter is read only");
    Require(FAILED(second->Write(input.data(), 1, &written)), "sealed Write rejected");
    Require(FAILED(second->SetLength(1)), "sealed SetLength rejected");
    Ok(second->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "clone full bytes");
    Require(bytes == input && source->Capacity() == capacity, "read clones share exact allocation");
    source.reset();
    output.reset(); // simulate ring eviction while decoder/clone leases remain
    Require(!lease.expired(), "eviction preserves decoder backing");
    ComPtr<IMFByteStream> afterEviction;
    Ok(clone(&afterEviction), "clone after source/ring eviction");
    clone = {};
    first.Reset();
    second.Reset();
    Require(!lease.expired(), "last decoder stream retains bytes");
    Ok(afterEviction->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "read evicted bytes");
    Require(read == input.size() && bytes == input, "evicted lease preserves exact compressed bytes");
    afterEviction.Reset();
    Require(lease.expired(), "last lease releases compressed allocation");
}

void RecorderSealFence() {
    VideoMemoryStream output(64);
    Ok(Access::Begin(output), "claim fresh memory output");
    Require(FAILED(Access::Begin(output)), "second recorder claim refused");
    Require(!output.Seal(), "recorder must finalize even with no live cursor");
    ComPtr<IStream> writer, clone;
    Ok(Access::Cursor(output, &writer), "recording cursor");
    constexpr BYTE value = 42;
    Ok(writer->Write(&value, 1, nullptr), "recording byte");
    Ok(writer->Clone(&clone), "retained writable recorder clone");
    Require(FAILED(Access::Retry(output)), "retry cannot clear live writer bytes");
    Access::Finish(output, true);
    Require(!output.Seal(), "finalize does not override retained writable clone");
    Require(writer->Write(&value, 1, nullptr) == STG_E_ACCESSDENIED,
        "stale writer revoked after recorder finalization");
    Require(clone->SetSize(Size(0)) == STG_E_ACCESSDENIED, "stale clone cannot truncate");
    ComPtr<IStream> late;
    Require(FAILED(clone->Clone(&late)), "stale clone cannot mint writer");
    writer.Reset(); clone.Reset();
    Require(output.Seal() != nullptr, "finalized cursor-free source seals");

    VideoMemoryStream failed;
    Ok(Access::Begin(failed), "claim failed output");
    Access::Finish(failed, false);
    Require(!failed.Seal(), "failed Finalize never publishes source");
    VideoMemoryStream full(1);
    Ok(Access::Begin(full), "claim small output");
    Ok(Access::Cursor(full, &writer), "small cursor");
    const BYTE two[]{1, 2};
    Require(FAILED(writer->Write(two, 2, nullptr)), "production cap failure");
    writer.Reset();
    Access::Finish(full, true);
    Require(!full.Seal(), "swallowed byte-stream write failure still prevents Seal");

    VideoMemoryStream retry(64);
    Ok(Access::Begin(retry), "claim retry output");
    Ok(Access::Cursor(retry, &writer), "failed-attempt cursor");
    Ok(writer->Write(two, 2, nullptr), "failed-attempt metadata");
    writer.Reset();
    const auto capacity = retry.Capacity();
    Ok(Access::Retry(retry), "reset quiesced failed attempt");
    Require(retry.ByteSize() == 0 && retry.Capacity() == capacity,
        "retry clears stale metadata without a second allocation");
    Access::Finish(retry, true);
    const auto retried = retry.Seal();
    Require(retried && retried->Capacity() == 0 && retry.Capacity() == 0,
        "empty successful retry releases unused allocation at Seal");
}

void CompactSegmentLeases() {
    constexpr std::size_t limit = 8u * 1024u * 1024u;
    // Two seconds at 16 Mbps plus 32 KiB of container margin. The parent
    // exercises the complete 150-segment ring; this checks actual per-source
    // allocation and byte-preserving read leases without duplicating the ring.
    constexpr std::size_t segmentBytes = 4'000'000u + 32u * 1024u;
    auto output = std::make_shared<VideoMemoryStream>(limit);
    ComPtr<IStream> writer;
    Ok(Access::Cursor(*output, &writer), "compact segment writer");
    std::array<BYTE, 64u * 1024u> block{};
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<BYTE>(i % 251);
    for (std::size_t position = 0; position < segmentBytes;) {
        const auto count = static_cast<ULONG>(std::min(block.size(), segmentBytes - position));
        Ok(writer->Write(block.data(), count, nullptr), "write nominal 16 Mbps segment");
        position += count;
    }
    Require(output->ByteSize() == segmentBytes && output->Capacity() == limit,
        "writable segment exposes actual full reservation");
    Require(!output->Seal() && output->Capacity() == limit,
        "live mutable lease prevents compaction");
    writer.Reset();
    auto source = output->Seal();
    Require(source && source->ByteSize() == segmentBytes && source->Capacity() == segmentBytes &&
        output->Capacity() == segmentBytes, "Seal replaces allocation, not capacity accounting");
    Require(source->Capacity() * 150u < 768ull * 1024u * 1024u,
        "actual compact per-segment capacity preserves nominal five-minute byte margin");
    ComPtr<IMFByteStream> first, second;
    okuflow::VideoReadStreamClone clone;
    Ok(source->OpenReadStream(&first, &clone), "open compact read leases");
    Ok(clone(&second), "clone compact source without byte copy");
    const auto identity = source.get();
    Require(output->Seal().get() == identity && source->Capacity() == segmentBytes,
        "repeated Seal and read cloning do not replace compact source");
    const auto backing = Access::Backing(*source);
    output.reset(); source.reset(); clone = {};
    first.Reset();
    Require(!backing.expired(), "compact backing survives source eviction");
    std::array<BYTE, 64u * 1024u> actual{};
    for (std::size_t position = 0; position < segmentBytes;) {
        const auto count = static_cast<ULONG>(std::min(actual.size(), segmentBytes - position));
        ULONG read{};
        Ok(second->Read(actual.data(), count, &read), "read complete compact segment lease");
        Require(read == count && std::equal(actual.begin(), actual.begin() + count, block.begin()),
            "compaction and evicted clone preserve every compressed byte");
        position += count;
    }
    second.Reset();
    Require(backing.expired(), "last compact decoder lease frees allocation");
}

void MemoryStartPolicy() {
    struct Case {
        UINT width, height, numerator, denominator, bitrate;
        bool valid;
    };
    const Case cases[]{
        {3840, 2160, 30, 1, 16'000'000, true},
        {2160, 3840, 30, 1, 16'000'000, true},
        {3840, 2160, 30'000, 1001, 16'000'000, true},
        {3840, 2160, 60, 2, 16'000'000, true},
        {3840, 2160, 31, 1, 16'000'000, false},
        {3840, 2160, 60, 1, 16'000'000, false},
        {3840, 2160, 30, 1, 8'000'000, false},
        {1920, 1080, 30, 1, 8'000'000, true},
        {1080, 1920, 30, 1, 8'000'000, true},
        {1920, 1080, 31, 1, 8'000'000, false},
        {1920, 1080, 30, 1, 16'000'000, false},
        {3840, 2160, 0, 1, 16'000'000, false},
        {3840, 2160, 30, 0, 16'000'000, false},
    };
    for (const auto& test : cases) {
        auto output = std::make_shared<VideoMemoryStream>();
        okuflow::VideoRecorder recorder;
        unsigned entries{};
        // Stop at the real memory sink boundary, before ConfigureSink creates
        // an encoder. This validates production 4K30/fractional-FPS policy
        // with no GPU and without claiming native 4K30 codec acceptance.
        okuflow::VideoRecorderTestAccess::ObserveSink(recorder,
            [&](bool gpu, IMFByteStream*) {
                Require(!gpu, "memory policy validation uses CPU input");
                ++entries;
                throw std::runtime_error("Intentional pre-encoder policy fixture stop");
            });
        Require(!recorder.StartMemory(output, test.width, test.height,
            test.numerator, test.denominator, test.bitrate), "policy fixture stops before encoder");
        Require(entries == (test.valid ? 1u : 0u), "production RAM FPS/bitrate policy");
        Require(!recorder.Archive() && recorder.OutputPath().empty() &&
            recorder.FailedAttemptPaths().empty(), "policy checks stay on memory route");
        if (test.valid) Require(!output->Seal(), "failed initialization cannot publish source");
        else {
            Require(output->Seal() != nullptr, "rejected parameters never claim output");
            Require(recorder.LastError().find("at most 30 FPS") != std::string::npos &&
                recorder.LastError().find("15 FPS") == std::string::npos,
                "RAM policy error copy reflects the common 30 FPS limit");
        }
    }
}

// Explicit opt-in codec seam. No path, camera, microphone, audio endpoint or
// native window is used. Default execution covers bounded streams only.
void NativeH264Seam() {
    constexpr UINT width = 320, height = 180;
    auto output = std::make_shared<VideoMemoryStream>();
    unsigned cpuAttempts{};
    okuflow::VideoRecorder recorder;
    okuflow::VideoRecorderTestAccess::ObserveSink(recorder,
        [&](bool gpu, IMFByteStream*) {
            Require(!gpu, "no GPU probe means CPU input");
            ++cpuAttempts;
            Require(!output->Seal(), "cannot seal during native initialization");
        });
    const bool started = recorder.StartMemory(output, width, height, 30, 1, 8'000'000);
    Require(started, recorder.LastError().c_str());
    Require(cpuAttempts == 1 && !recorder.Archive() && recorder.OutputPath().empty() &&
        recorder.FailedAttemptPaths().empty(), "RAM route has no path/archive/file attempts");
    Require(!output->Seal(), "native writer must finalize before Seal");
    std::vector<BYTE> pixels(width * height * 4u, 255);
    constexpr std::int64_t origin = 120'000'000;
    constexpr int count = 18;
    std::array<std::int64_t, count> expected{};
    for (int i = 0; i < count; ++i) {
        expected[i] = static_cast<std::int64_t>(i) * 10'000'000 / 30 +
            (i >= 9 ? 7'000'000 : 0); // retain an actual 700 ms delivery gap
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!recorder.CanAcceptVideoFrame() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool admitted=recorder.CanAcceptVideoFrame();
        if(!admitted)okuflow::VideoRecorderTestAccess::PrintAdmission(recorder,i);
        Require(admitted, "bounded native encoder admission");
        for (std::size_t p = 0; p < pixels.size(); p += 4) {
            pixels[p] = static_cast<BYTE>(i * 7);
            pixels[p + 1] = static_cast<BYTE>((p / 4 + i * 5) % 200);
            pixels[p + 2] = static_cast<BYTE>(200 - i * 5);
        }
        okuflow::RecordingFrameIdentity identity;
        identity.captureTimestamp100ns = origin + expected[i];
        identity.captureClock100ns = identity.captureTimestamp100ns;
        identity.sequenceNumber = i + 1;
        const bool added = recorder.AddFrame(pixels.data(), width * 4u, identity);
        Require(added, recorder.LastError().c_str());
        Require(recorder.LastAdmittedVideoTimestamp100ns() == expected[i], "delivery gaps remain exact");
        if (i == 0) {
            Require(!recorder.AddFrame(pixels.data(), width * 4u, identity), "duplicate acquisition refused");
            identity.captureTimestamp100ns = -1;
            Require(!recorder.AddFrame(pixels.data(), width * 4u, identity), "unknown acquisition refused");
            Require(recorder.IsRecording() && recorder.LastAdmittedVideoTimestamp100ns() == 0,
                "rejected timestamps do not advance timeline");
        }
    }
    const auto stopped = recorder.Stop();
    Require(stopped.HasPlayableVideo() && stopped.videoSamplesWritten == count,
        "native memory writer finalized all samples");
    auto source = output->Seal();
    Require(source && source->ByteSize() > 0 && source->Capacity() == source->ByteSize() &&
        source->Capacity() <= 8u * 1024u * 1024u,
        "native finalization produces bounded sealed bytes");
    ComPtr<IMFByteStream> bytes;
    Ok(source->OpenReadStream(&bytes), "open native output bytes");
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFAttributes> attributes;
    Ok(MFCreateAttributes(&attributes, 1), "native CPU reader attributes");
    Ok(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE),
        "CPU-only native decode");
    Ok(MFCreateSourceReaderFromByteStream(bytes.Get(), attributes.Get(), &reader), "read native RAM fMP4");
    ComPtr<IMFMediaType> native;
    Ok(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native), "native video track");
    GUID subtype{};
    Ok(native->GetGUID(MF_MT_SUBTYPE, &subtype), "encoded subtype");
    Require(subtype == MFVideoFormat_H264, "memory output is H264");
    ComPtr<IMFMediaType> audio;
    Require(FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio)),
        "memory output is video only");
    // Inspect a real compressed sample before requesting decoded NV12. Every
    // fresh encoder/segment must start with an actual random-access picture.
    ComPtr<IMFSample> firstCompressed;
    DWORD initialFlags{};
    LONGLONG initialTimestamp{};
    for (int reads = 0; reads < 32 && !firstCompressed; ++reads) {
        Ok(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
            &initialFlags, &initialTimestamp, &firstCompressed), "first native compressed sample");
        Require(!(initialFlags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)),
            "initial native compressed video available");
    }
    UINT32 cleanPoint{};
    Require(firstCompressed && SUCCEEDED(firstCompressed->GetUINT32(
        MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint,
        "new memory segment begins with actual keyframe");
    firstCompressed.Reset();
    reader.Reset();
    bytes.Reset();
    Ok(source->OpenReadStream(&bytes), "fresh cursor over same encoded allocation");
    Ok(MFCreateSourceReaderFromByteStream(bytes.Get(), attributes.Get(), &reader),
        "fresh native decoder cursor");
    ComPtr<IMFMediaType> decoded;
    Ok(MFCreateMediaType(&decoded), "decoded type");
    Ok(decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "decoded major");
    Ok(decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "decoded NV12");
    Ok(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, decoded.Get()),
        "select actual native decoder");
    output.reset(); source.reset(); // decoder's byte-stream lease owns evicted bytes
    int frames{};
    bool eos{};
    for (int reads = 0; reads < 256 && !eos; ++reads) {
        DWORD flags{};
        LONGLONG timestamp{};
        ComPtr<IMFSample> sample;
        Ok(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
            &flags, &timestamp, &sample), "decode memory sample");
        Require(!(flags & MF_SOURCE_READERF_ERROR), "native reader error");
        if (sample) {
            if(frames>=count || std::llabs(timestamp-expected[frames])>1)
                std::cerr<<"decoded PTS mismatch frame="<<frames<<" actual="<<timestamp<<" expected="
                    <<(frames<count?expected[frames]:-1)<<'\n';
            Require(frames < count && std::llabs(timestamp - expected[frames]) <= 1,
                "decoded PTS preserves delivery gap, no invented frames");
            ComPtr<IMFMediaBuffer> buffer;
            Ok(sample->ConvertToContiguousBuffer(&buffer), "decoded actual pixels");
            DWORD length{};
            Ok(buffer->GetCurrentLength(&length), "decoded pixel length");
            Require(length >= width * height * 3u / 2u, "native decoded NV12 bytes");
            ++frames;
        }
        eos = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
    }
    Require(eos && frames == count, "decode complete synthetic memory segment");
}

} // namespace

int main(int argc, char** argv) {
    const bool native = argc == 2 && std::string_view(argv[1]) == "--native-h264";
    if (argc > 1 && !native) {
        std::cerr << "Usage: video_memory_stream_tests [--native-h264]\n";
        return 2;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mf)) { CoUninitialize(); return 1; }
    int result{};
    try {
        BoundsAndCursors();
        AdapterAndEvictedLeases();
        RecorderSealFence();
        CompactSegmentLeases();
        MemoryStartPolicy();
        if (native) NativeH264Seam();
        std::cout << "Bounded memory stream contracts passed" <<
            (native ? "; native video-only H264 RAM seam passed\n" : "; native seam not requested\n");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    MFShutdown();
    CoUninitialize();
    return result;
}

#endif // _WIN32
