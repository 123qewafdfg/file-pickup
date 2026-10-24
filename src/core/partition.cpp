#include "core/partition.h"

#include <algorithm>

namespace cj {

namespace qq {

QList<qint64> plan(qint64 bytes)
{
    QList<qint64> result;
    if (bytes <= 0)
        return result;

    const qint64 partCount = (bytes + kPayloadBytesPerImage - 1) / kPayloadBytesPerImage;
    if (partCount > 65535)
        return result;

    qint64 remaining = bytes;
    while (remaining > 0) {
        const qint64 size = std::min(kPayloadBytesPerImage, remaining);
        result.append(size);
        remaining -= size;
    }
    return result;
}

} // namespace qq

namespace bili {

QList<qint64> plan(qint64 bytes, int metadataBytes, qint64 carrierBytes)
{
    QList<qint64> result;
    if (bytes <= 0 || bytes > kMaxPayload)
        return result;

    const qint64 limit = std::min(kMaxAppend, kMaxCarrierBytes - carrierBytes);
    if (limit <= kOverhead)
        return result;

    for (int count = 1; count <= kMaxParts; ++count) {
        QList<qint64> bases;
        bases.reserve(count);
        for (int i = 0; i < count; ++i)
            bases.append(kOverhead + (i == 0 ? metadataBytes : 0));

        qint64 capacity = 0;
        for (qint64 base : bases)
            capacity += std::max<qint64>(0, limit - base);
        if (capacity < bytes)
            continue;

        qint64 low = *std::max_element(bases.begin(), bases.end());
        qint64 high = limit;
        while (low < high) {
            const qint64 mid = low + (high - low) / 2;
            qint64 sum = 0;
            for (qint64 base : bases)
                sum += std::max<qint64>(0, mid - base);
            if (sum >= bytes)
                high = mid;
            else
                low = mid + 1;
        }

        QList<qint64> sizes;
        sizes.reserve(count);
        for (qint64 base : bases)
            sizes.append(std::max<qint64>(0, low - base));

        qint64 excess = 0;
        for (qint64 size : sizes)
            excess += size;
        excess -= bytes;

        int index = sizes.size() - 1;
        while (excess > 0) {
            if (sizes[index] > 0) {
                --sizes[index];
                --excess;
            }
            index = (index == 0) ? sizes.size() - 1 : index - 1;
        }
        return sizes;
    }
    return result;
}

} // namespace bili

} // namespace cj
