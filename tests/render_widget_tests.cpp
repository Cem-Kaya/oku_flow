#include "openzoom/ui/render_widget.hpp"
#include "openzoom/d3d12/presenter.hpp"

#include <QElapsedTimer>
#include <QTimer>
#include <QtTest>

#include <algorithm>

namespace openzoom {
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
ComPtr<ID3D12Resource> CircleTexture(ID3D12Device* device)
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
            row[x * 4 + 3] = 255;
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
    QTest::qWait(220);
    producer.stop();
    QVERIFY(changes >= 10);
    QVERIFY(updatedWhileMoving);
    QTRY_COMPARE(QSize(presenter.ViewportWidth(), presenter.ViewportHeight()), NativeSize(widget));
    QVERIFY(presenter.WaitForIdle());
}

void RenderWidgetTests::circleKeepsItsAspectAcrossViewportResizes_data()
{
    QTest::addColumn<bool>("fit");
    QTest::newRow("fill") << false;
    QTest::newRow("fit") << true;
}

void RenderWidgetTests::circleKeepsItsAspectAcrossViewportResizes()
{
    QFETCH(bool, fit);
    if (!D3D12Available()) QSKIP("D3D12 is unavailable");
    D3D12Presenter presenter;
    RenderWidget widget;
    widget.setPresenter(&presenter);
    widget.resize(800, 450);
    widget.show();
    QVERIFY(widget.isPresenterReady());
    auto texture = CircleTexture(presenter.GetDevice());
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
                if (pixels[(static_cast<std::size_t>(y) * width + x) * 4] > 180) {
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
} // namespace openzoom

QTEST_MAIN(openzoom::RenderWidgetTests)
#include "render_widget_tests.moc"
