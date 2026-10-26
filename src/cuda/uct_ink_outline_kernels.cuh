// First-party contour extraction. No coverage grid or source image leaves CUDA.
namespace okuflow::uct::gpu
{
namespace
{
__global__ void inkCoverage(const unsigned char *image, std::size_t pitch, ink::Region region,
                            unsigned char *coverage, ink::Summary *summary)
{
    __shared__ int histogram[256];
    histogram[threadIdx.x] = 0;
    __syncthreads();
    for (int i = threadIdx.x; i < region.w * region.h; i += blockDim.x)
    {
        const auto q =
            ink::Coverage(image + (region.y + i / region.w) * pitch + (region.x + i % region.w) * 4, region);
        coverage[i] = q;
        atomicAdd(histogram + q, 1);
    }
    __syncthreads();
    if (threadIdx.x == 0)
        *summary = ink::Thresholds(histogram, region.w * region.h, region);
}
__global__ void inkCounts(const unsigned char *coverage, ink::Region region, int *prefixes, int *rows,
                          ink::Summary *summary)
{
    if (summary->invalid == 1)
        return; // immutable threshold refusal; other blocks may set 2
    __shared__ int warpTotals[16];
    const int x = threadIdx.x, y = blockIdx.x, k = blockIdx.y;
    const int cells = region.w - 1, cell = y * cells + x;
    int count = 0;
    if (x < cells)
    {
        const int at = y * region.w + x;
        const int q[]{coverage[at], coverage[at + 1], coverage[at + region.w + 1], coverage[at + region.w]};
        count = ink::Cell(q, summary->cuts[k], x, y, nullptr);
        if (count < 0)
        {
            atomicExch(&summary->invalid, 2);
            count = 0;
        }
        if (k == 0 && ((y == 0 && (q[0] > summary->cuts[0] || q[1] > summary->cuts[0])) ||
                       (y + 2 == region.h && (q[2] > summary->cuts[0] || q[3] > summary->cuts[0])) ||
                       (x == 0 && (q[0] > summary->cuts[0] || q[3] > summary->cuts[0])) ||
                       (x + 2 == region.w && (q[1] > summary->cuts[0] || q[2] > summary->cuts[0]))))
            atomicExch(&summary->border, 1);
    }
    int inclusive = count;
    for (int offset = 1; offset < 32; offset *= 2)
    {
        const int previous = __shfl_up_sync(0xffffffff, inclusive, offset);
        if ((threadIdx.x & 31) >= offset)
            inclusive += previous;
    }
    const int lane = threadIdx.x & 31, warp = threadIdx.x / 32;
    if (lane == 31)
        warpTotals[warp] = inclusive;
    __syncthreads();
    if (warp == 0)
    {
        int sum = lane < 16 ? warpTotals[lane] : 0;
        for (int offset = 1; offset < 32; offset *= 2)
        {
            const int previous = __shfl_up_sync(0xffffffff, sum, offset);
            if (lane >= offset)
                sum += previous;
        }
        if (lane < 16)
            warpTotals[lane] = sum;
    }
    __syncthreads();
    if (x < cells)
        prefixes[k * (region.w - 1) * (region.h - 1) + cell] =
            (warp ? warpTotals[warp - 1] : 0) + inclusive - count;
    if (threadIdx.x == 511)
        rows[k * (region.h - 1) + y] = warpTotals[15];
}
__global__ void inkRowOffsets(int height, int *rows, ink::Summary *summary)
{
    if (summary->invalid)
        return;
    for (int k = 0; k < ink::Levels; ++k)
    {
        int offset = 0;
        for (int y = 0; y < height - 1; ++y)
        {
            const int count = rows[k * (height - 1) + y];
            rows[k * (height - 1) + y] = offset;
            offset += count;
        }
        summary->counts[k] = offset;
    }
}
__global__ void inkSegments(const unsigned char *coverage, ink::Region region, const int *prefixes,
                            const int *rows, const ink::Summary *summary, ink::Segment *output)
{
    if (summary->invalid)
        return;
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y, k = blockIdx.z;
    if (x + 1 >= region.w || summary->counts[k] > ink::MaximumSegments)
        return;
    const int at = y * region.w + x;
    const int q[]{coverage[at], coverage[at + 1], coverage[at + region.w + 1], coverage[at + region.w]};
    ink::Segment segments[2];
    const int count = ink::Cell(q, summary->cuts[k], x, y, segments);
    const int offset =
        rows[k * (region.h - 1) + y] + prefixes[k * (region.w - 1) * (region.h - 1) + y * (region.w - 1) + x];
    for (int i = 0; i < count; ++i)
        output[k * ink::MaximumSegments + offset + i] = segments[i];
}
} // namespace
void InkOutline(const void *image, std::size_t pitch, int width, int height, ink::Region region,
                unsigned char *coverage, int *prefixes, int *rows, ink::Summary *summary,
                ink::Segment *segments, cudaStream_t stream)
{
    for (int c = 0; c < 3; ++c)
        if (region.foreground[c] < 0 || region.foreground[c] > 255 || region.background[c] < 0 ||
            region.background[c] > 255)
            throw std::invalid_argument("Unsupported source-ink colours.");
    if (!image || !coverage || !prefixes || !rows || !summary || !segments || region.x < 0 || region.y < 0 ||
        region.w < 12 || region.h < 12 || region.w > ink::MaximumWidth || region.h > ink::MaximumHeight ||
        region.w * region.h > ink::MaximumPixels || width < region.w || height < region.h ||
        region.x > width - region.w || region.y > height - region.h || pitch < std::size_t(width) * 4)
        throw std::invalid_argument("Unsupported source-ink outline region.");
    inkCoverage<<<1, 256, 0, stream>>>(static_cast<const unsigned char *>(image), pitch, region, coverage,
                                       summary);
    inkCounts<<<dim3(region.h - 1, ink::Levels), 512, 0, stream>>>(coverage, region, prefixes, rows, summary);
    inkRowOffsets<<<1, 1, 0, stream>>>(region.h, rows, summary);
    inkSegments<<<dim3((region.w - 1 + 127) / 128, region.h - 1, ink::Levels), 128, 0, stream>>>(
        coverage, region, prefixes, rows, summary, segments);
}
} // namespace okuflow::uct::gpu
