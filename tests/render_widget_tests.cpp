#include "gui_test_support.hpp"
#include "okuflow/ui/render_widget.hpp"
#include "okuflow/d3d12/presenter.hpp"

#include <QElapsedTimer>
#include <QTimer>
#include <QtTest>

#include <algorithm>

namespace okuflow {
namespace {
using Microsoft::WRL::ComPtr;

bool D3D12Available()
{
    return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                       __uuidof(ID3D12Device), nullptr));
}

QSize NativeSize(QWidget& widget)
{
    RECT client{};
    GetClientRect(reinterpret_cast<HWND>(widget.winId()), &client);
    return {client.right - client.left, client.bottom - client.top};
}

// Upload a known circular target through a separate queue, then wait for it
// before handing the COMMON-state texture to the production presenter.
ComPtr<ID3D12Resource> CircleTexture(ID3D12Device* device, std::uint8_t alpha = 255)
{
    ComPtr<ID3D12Resource> texture, upload;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 320;
    desc.Height = 180;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&texture)))) return {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = totalBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload)))) return {};
    std::uint8_t* pixels{};
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&pixels)))) return {};
    for (int y = 0; y < 180; ++y) {
        auto* row = pixels + footprint.Offset + y * footprint.Footprint.RowPitch;
        for (int x = 0; x < 320; ++x) {
            const int dx = x - 160;
            const int dy = y - 90;
            const std::uint8_t value = dx * dx + dy * dy <= 24 * 24 ? 255 : 0;
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = value;
            row[x * 4 + 3] = alpha;
        }
    }
    upload->Unmap(0, nullptr);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                          nullptr, IID_PPV_ARGS(&commands))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return {};
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close())) return {};
    HANDLE completed = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!completed) return {};
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1)) ||
        FAILED(fence->SetEventOnCompletion(1, completed)) ||
        WaitForSingleObject(completed, 2000) != WAIT_OBJECT_0) {
        // A failed completion is not permission to destroy submitted objects.
        texture.Detach(); upload.Detach(); queue.Detach(); allocator.Detach();
        commands.Detach(); fence.Detach();
        return {};
    }
    CloseHandle(completed);
    return texture;
}
} // namespace

class RenderWidgetTests final : public QObject {
    Q_OBJECT
private slots:
    void continuousResizingDoesNotStarvePresenter();
    void circleKeepsItsAspectAcrossViewportResizes_data();
    void circleKeepsItsAspectAcrossViewportResizes();
    void recordingAnnotationsPreserveTransparency();
};

void RenderWidgetTests::continuousResizingDoesNotStarvePresenter()
{
    if (!D3D12Available()) QSKIP("D3D12 is unavailable");
    D3D12Presenter presenter;
    RenderWidget widget;
    widget.setPresenter(&presenter);
    widget.resize(800, 450);
    widget.show();
    QVERIFY(widget.isPresenterReady());
    const UINT initialWidth = presenter.ViewportWidth();
    int changes = 0;
    bool updatedWhileMoving = false;
    QTimer producer;
    producer.setTimerType(Qt::PreciseTimer);
    producer.setInterval(2);
    connect(&producer, &QTimer::timeout, &widget, [&]() {
        updatedWhileMoving |= presenter.ViewportWidth() != initialWidth;
        widget.resize(500 + (++changes % 180), 450);
    });
    producer.start();
    // The contract is a presenter update while the resize producer is still
    // active, after at least ten changes. Do not spend the full deadline once
    // both conditions are observed; retain the same 220 ms failure deadline.
    const bool progressed = QTest::qWaitFor([&] {
        return changes >= 10 && updatedWhileMoving;
    }, 220);
    producer.stop();
    QVERIFY(progressed);
    QVERIFY(changes >= 10);
    QVERIFY(updatedWhileMoving);
    QTRY_COMPARE(QSize(presenter.ViewportWidth(), presenter.ViewportHeight()), NativeSize(widget));
    QVERIFY(presenter.WaitForIdle());
}

void RenderWidgetTests::circleKeepsItsAspectAcrossViewportResizes_data()
{
    QTest::addColumn<bool>("fit");
    QTest::addColumn<int>("sourceAlpha");
    QTest::newRow("fill") << false << 255;
    QTest::newRow("fit") << true << 255;
    QTest::newRow("fill-zero-alpha") << false << 0;
    QTest::newRow("fit-zero-alpha") << true << 0;
}

void RenderWidgetTests::circleKeepsItsAspectAcrossViewportResizes()
{
    QFETCH(bool, fit);
    QFETCH(int, sourceAlpha);
    if (!D3D12Available()) QSKIP("D3D12 is unavailable");
    D3D12Presenter presenter;
    RenderWidget widget;
    widget.setPresenter(&presenter);
    widget.resize(800, 450);
    widget.show();
    QVERIFY(widget.isPresenterReady());
    auto texture = CircleTexture(presenter.GetDevice(), static_cast<std::uint8_t>(sourceAlpha));
    QVERIFY(texture);
    for (const QSize size : {QSize(800, 450), QSize(500, 450), QSize(1000, 450), QSize(320, 500)}) {
        widget.resize(size);
        QTRY_COMPARE(QSize(presenter.ViewportWidth(), presenter.ViewportHeight()), NativeSize(widget));
        const auto transform = ComputeViewTransform(320, 180, presenter.ViewportWidth(),
                                                     presenter.ViewportHeight(), 1.0f, 0.5f, 0.5f,
                                                     fit ? ViewportFitMode::kFit : ViewportFitMode::kFill);
        ViewportPresentationOptions options;
        options.requestReadback = true;
        UINT64 requestId{};
        // Frame-slot admission is nonblocking. Retry busy slots and remember
        // success so QTRY_VERIFY's final evaluation cannot submit twice.
        bool submitted = false;
        QTRY_VERIFY_WITH_TIMEOUT(submitted ||
            (submitted = presenter.PresentSceneTexture(texture.Get(), 320, 180, transform,
                                                       nullptr, &options, &requestId)), 2000);
        QVERIFY(requestId != 0);
        std::vector<std::uint8_t> pixels;
        UINT width{}, height{};
        UINT64 completedId{};
        // QTRY_VERIFY evaluates its expression again after success. Remember
        // completion because polling consumes the completed readback slot.
        bool completed = false;
        QTRY_VERIFY_WITH_TIMEOUT(completed ||
            (completed = presenter.TryGetCompletedReadback(pixels, width, height, &completedId)), 2000);
        QCOMPARE(completedId, requestId);
        QCOMPARE(QSize(width, height), NativeSize(widget));
        int minX = static_cast<int>(width), minY = static_cast<int>(height), maxX = -1, maxY = -1;
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
                QCOMPARE(pixels[offset + 3], std::uint8_t{255});
                if (pixels[offset] > 180) {
                    minX = std::min(minX, static_cast<int>(x));
                    maxX = std::max(maxX, static_cast<int>(x));
                    minY = std::min(minY, static_cast<int>(y));
                    maxY = std::max(maxY, static_cast<int>(y));
                }
            }
        }
        QVERIFY(maxX > minX);
        QVERIFY(maxY > minY);
        QVERIFY2(std::abs((maxX - minX) - (maxY - minY)) <= 3,
                 "The circular source was stretched non-uniformly after a viewport resize");
    }
    QVERIFY(presenter.WaitForIdle());
}
void RenderWidgetTests::recordingAnnotationsPreserveTransparency()
{
    if (!D3D12Available()) QSKIP("D3D12 is unavailable");
    D3D12Presenter presenter;
    RenderWidget widget;
    widget.setPresenter(&presenter);
    widget.resize(640, 360);
    widget.show();
    QVERIFY(widget.isPresenterReady());
    // The camera may have bogus alpha; recording still needs an opaque scene.
    auto scene = CircleTexture(presenter.GetDevice(), 0);
    QVERIFY(scene);
    constexpr UINT width = 320, height = 180;
    RecordingViewTransform transform;
    transform.valid = true;
    auto cleanFrame = presenter.RequestRecordingFrame(scene.Get(), width, height,
                                                       transform, width, height);
    QVERIFY(cleanFrame.IsValid());
    ComPtr<ID3D12Resource> cleanTexture;
    QVERIFY(SUCCEEDED(presenter.GetDevice()->OpenSharedHandle(cleanFrame.textureSharedHandle,
                                                               IID_PPV_ARGS(&cleanTexture))));
    std::vector<std::uint8_t> clean;
    QVERIFY(presenter.ReadbackTexture(cleanTexture.Get(), width, height, clean,
                                      cleanFrame.readyFenceValue));

    std::vector<std::uint8_t> annotation(static_cast<std::size_t>(width) * height * 4u, 0);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            if (x >= 152 && x < 169 && y >= 82 && y < 99) {
                // Half-opacity red over the white camera target.
                annotation[offset + 2] = 255;
                annotation[offset + 3] = 128;
            } else if (x >= 16 && x < 32 && y >= 16 && y < 32) {
                annotation[offset] = 10;
                annotation[offset + 1] = 80;
                annotation[offset + 2] = 220;
                annotation[offset + 3] = 255;
            } else if (x < 8 && y < 8) {
                // Transparent RGB must have no influence on the camera.
                annotation[offset] = 7;
                annotation[offset + 1] = 230;
                annotation[offset + 2] = 61;
            }
        }
    }
    auto inkFrame = presenter.RequestRecordingFrame(scene.Get(), width, height,
        transform, width, height, annotation.data(), width * 4u);
    QVERIFY(inkFrame.IsValid());
    ComPtr<ID3D12Resource> inkTexture;
    QVERIFY(SUCCEEDED(presenter.GetDevice()->OpenSharedHandle(inkFrame.textureSharedHandle,
                                                               IID_PPV_ARGS(&inkTexture))));
    std::vector<std::uint8_t> composed;
    QVERIFY(presenter.ReadbackTexture(inkTexture.Get(), width, height, composed,
                                      inkFrame.readyFenceValue));
    QCOMPARE(clean.size(), annotation.size());
    QCOMPARE(composed.size(), annotation.size());
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            QCOMPARE(clean[offset + 3], std::uint8_t{255});
            QCOMPARE(composed[offset + 3], std::uint8_t{255});
            const int alpha = annotation[offset + 3];
            for (int channel = 0; channel < 3; ++channel) {
                const double expected = (annotation[offset + channel] * alpha +
                    clean[offset + channel] * (255 - alpha)) / 255.0;
                QVERIFY2(std::abs(composed[offset + channel] - expected) <= 1.0,
                         "Recording changed transparent background or translucent/opaque ink blending");
            }
        }
    }
    // Explicit samples distinguish a transparent canvas from opaque black,
    // and translucent red from the fully opaque red produced by the regression.
    const std::size_t clearWhite = (90u * width + 142u) * 4u;
    QCOMPARE(composed[clearWhite], std::uint8_t{255});
    const std::size_t translucent = (90u * width + 160u) * 4u;
    QVERIFY(composed[translucent] >= 126 && composed[translucent] <= 128);
    QVERIFY(composed[translucent + 1] >= 126 && composed[translucent + 1] <= 128);
    QCOMPARE(composed[translucent + 2], std::uint8_t{255});
    QVERIFY(presenter.WaitForIdle());
}

} // namespace okuflow

OKUFLOW_GUI_TEST_MAIN(okuflow::RenderWidgetTests)
#include "render_widget_tests.moc"
