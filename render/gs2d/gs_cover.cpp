#include "gs_cover.h"

#include <algorithm>
#include <cstring>

namespace rc::rd
{
    namespace
    {
        int64_t floorDiv(int64_t n, int64_t d) // d > 0
        {
            int64_t q = n / d;
            if ((n % d) != 0 && n < 0)
                --q;
            return q;
        }
        int64_t ceilDiv(int64_t n, int64_t d) { return -floorDiv(-n, d); } // d > 0
    }

    GsCoverMap::GsCoverMap()
    {
        m_stamp[0].assign(static_cast<size_t>(kWidth) * kHeight, 0);
        m_stamp[1].assign(static_cast<size_t>(kWidth) * kHeight, 0);
    }

    uint64_t GsCoverMap::triangle(const GsTri &t, int x0, int y0, int x1, int y1, std::vector<GsSpan> &out)
    {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, kWidth - 1);
        y1 = std::min(y1, kHeight - 1);
        uint64_t n = 0;
        for (int y = y0; y <= y1; ++y)
        {
            // Covered where a_i * 16x + (b_i * 16y + c_i + bias_i) > 0 for every edge (gsTriCovers).
            int64_t lo = x0, hi = x1;
            for (int i = 0; i < 3 && lo <= hi; ++i)
            {
                const int64_t A = t.a[i] * 16;
                const int64_t K = t.b[i] * (static_cast<int64_t>(y) * 16) + t.c[i] + t.bias[i];
                if (A > 0)
                    lo = std::max(lo, floorDiv(-K, A) + 1);
                else if (A < 0)
                    hi = std::min(hi, ceilDiv(K, -A) - 1);
                else if (K <= 0)
                    hi = lo - 1;
            }
            if (lo > hi)
                continue;
            out.push_back({static_cast<int16_t>(y), static_cast<int16_t>(lo), static_cast<int16_t>(hi)});
            n += static_cast<uint64_t>(hi - lo + 1);
        }
        return n;
    }

    uint64_t GsCoverMap::rect(int x0, int y0, int x1, int y1, std::vector<GsSpan> &out)
    {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, kWidth - 1);
        y1 = std::min(y1, kHeight - 1);
        if (x1 < x0 || y1 < y0)
            return 0;
        for (int y = y0; y <= y1; ++y)
            out.push_back({static_cast<int16_t>(y), static_cast<int16_t>(x0), static_cast<int16_t>(x1)});
        return static_cast<uint64_t>(x1 - x0 + 1) * static_cast<uint64_t>(y1 - y0 + 1);
    }

    void GsCoverMap::beginPass()
    {
        advance(kRead);
        advance(kSplit);
    }

    bool GsCoverMap::any(const GsSpan *spans, size_t n, Plane p) const
    {
        const uint16_t e = m_epoch[p];
        const uint16_t *s = m_stamp[p].data();
        for (size_t i = 0; i < n; ++i)
        {
            const uint16_t *row = s + static_cast<size_t>(spans[i].y) * kWidth;
            m_visited += static_cast<uint64_t>(spans[i].x1 - spans[i].x0 + 1);
            for (int x = spans[i].x0; x <= spans[i].x1; ++x)
                if (row[x] == e)
                    return true;
        }
        return false;
    }

    void GsCoverMap::stamp(const GsSpan *spans, size_t n, Plane p)
    {
        const uint16_t e = m_epoch[p];
        uint16_t *s = m_stamp[p].data();
        for (size_t i = 0; i < n; ++i)
        {
            uint16_t *row = s + static_cast<size_t>(spans[i].y) * kWidth;
            m_visited += static_cast<uint64_t>(spans[i].x1 - spans[i].x0 + 1);
            std::fill(row + spans[i].x0, row + spans[i].x1 + 1, e);
        }
    }

    void GsCoverMap::advance(Plane p)
    {
        if (++m_epoch[p] == 0)
        {
            // The 16-bit epoch wrapped: clear the plane so no old stamp can equal a new epoch.
            std::fill(m_stamp[p].begin(), m_stamp[p].end(), 0);
            m_epoch[p] = 1;
        }
    }
}
