#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace winrings
{
    inline constexpr double kPi = 3.14159265358979323846;
    inline constexpr double kTwoPi = 2.0 * kPi;

    struct PointD
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct RectD
    {
        double left = 0.0;
        double top = 0.0;
        double width = 0.0;
        double height = 0.0;

        constexpr double Right() const noexcept { return left + width; }
        constexpr double Bottom() const noexcept { return top + height; }
        constexpr bool IsEmpty() const noexcept { return width <= 0.0 || height <= 0.0; }
        constexpr bool Contains(PointD pt) const noexcept
        {
            return pt.x >= left && pt.x <= Right() && pt.y >= top && pt.y <= Bottom();
        }
    };

    /// Computes active sector index (0..slotCount-1) anchored at 12 o'clock, increasing clockwise.
    /// Returns -1 inside deadzoneRadius (e.g. 18 DIP) or when slotCount <= 0.
    inline int CalculateSectorIndex(double dx, double dy, int slotCount, double deadzoneRadius) noexcept
    {
        if (slotCount <= 0)
            return -1;

        const double distance = std::hypot(dx, dy);
        if (distance <= deadzoneRadius)
            return -1;

        double angle = std::fmod(std::atan2(dy, dx) + kPi / 2.0 + kTwoPi, kTwoPi);
        if (angle < 0.0)
            angle += kTwoPi;

        const double sectorWidth = kTwoPi / static_cast<double>(slotCount);
        double shifted = std::fmod(angle + sectorWidth / 2.0, kTwoPi);
        if (shifted < 0.0)
            shifted += kTwoPi;

        const int index = static_cast<int>(std::floor(shifted / sectorWidth));
        return std::clamp(index, 0, slotCount - 1);
    }

    inline double GetSlotAngle(int index, int slotCount) noexcept
    {
        if (slotCount <= 0)
            return -kPi / 2.0;
        return -kPi / 2.0 + (kTwoPi * static_cast<double>(index) / static_cast<double>(slotCount));
    }

    inline PointD GetSlotCenter(double centerX, double centerY, double radius, int index, int slotCount) noexcept
    {
        const double angle = GetSlotAngle(index, slotCount);
        return {
            centerX + radius * std::cos(angle),
            centerY + radius * std::sin(angle)
        };
    }

    /// Computes fanned-out outer arc centers for up to 9 sub-items.
    /// Index 0 sits directly outward along baseAngleRad; siblings fan symmetrically (+1, -1, +2, -2, ...).
    inline std::vector<PointD> GetOuterArcCenters(
        double centerX,
        double centerY,
        double baseAngleRad,
        double outerRadius,
        int itemCount,
        double angularStepRad = 0.34)
    {
        const int count = std::clamp(itemCount, 0, 9);
        std::vector<PointD> points;
        points.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            const int offset = (i == 0) ? 0 : ((i + 1) / 2) * ((i % 2 == 1) ? 1 : -1);
            const double angle = baseAngleRad + static_cast<double>(offset) * angularStepRad;
            points.push_back({
                centerX + outerRadius * std::cos(angle),
                centerY + outerRadius * std::sin(angle)
            });
        }
        return points;
    }

    inline double NormalizeAngleDelta(double radians) noexcept
    {
        while (radians > kPi) radians -= kTwoPi;
        while (radians < -kPi) radians += kTwoPi;
        return radians;
    }

    struct RingGeometry
    {
        double iconSize = 46.0;
        double ringRadius = 114.6;
        double outerRadius = 149.6;
        double holeRadius = 81.6;
        double bandRadius = 91.6;
        double runningDotRadius = 85.6;
    };

    inline double MainRingOuterRadius(double ringRadiusDip, double iconSizeDip) noexcept
    {
        return ringRadiusDip + iconSizeDip * 0.5 + 12.0;
    }

    inline RingGeometry ComputeRingGeometry(
        int slotCount,
        double configRingRadiusDip,
        double configIconSizeDip,
        double deadzoneDip) noexcept
    {
        RingGeometry g{};
        double icon = std::clamp(configIconSizeDip, 32.0, 56.0);
        if (slotCount >= 13)
            icon = (std::min)(icon, 40.0);
        else if (slotCount >= 9)
            icon = (std::min)(icon, 46.0);

        const double minNeeded = (slotCount > 0)
            ? ((icon + 14.0) * static_cast<double>(slotCount)) / kTwoPi
            : 88.0;
        g.iconSize = icon;
        g.ringRadius = (std::max)({ 88.0, configRingRadiusDip, minNeeded });
        g.outerRadius = MainRingOuterRadius(g.ringRadius, g.iconSize);
        g.holeRadius = (std::max)(deadzoneDip + 8.0, g.ringRadius - g.iconSize * 0.5 - 10.0);
        g.bandRadius = (std::max)(g.holeRadius + 4.0, g.ringRadius - g.iconSize * 0.5);
        g.runningDotRadius = (std::max)(g.holeRadius + 4.0, g.ringRadius - g.iconSize * 0.5 - 6.0);
        return g;
    }

    struct IconArcLayout
    {
        double arcRadius = 0.0;
        double startAngleRad = 0.0;
        double endAngleRad = 0.0;
        double trackWidthDip = 56.0;
        std::vector<double> angles;
        std::vector<RectD> rects;
    };

    /// Computes a continuous curved half-ring icon layout (`40x40 DIP` items on a `56 DIP` wide arc track).
    inline IconArcLayout ComputeIconArcLayout(
        double centerX,
        double centerY,
        double baseAngleRad,
        double mainRingOuterR,
        int itemCount,
        double itemSizeDip = 40.0,
        double clearanceDip = 12.0,
        double maxSpanRad = 4.01426,
        bool alternatingFromCenter = false)
    {
        IconArcLayout out{};
        const int count = std::clamp(itemCount, 0, 12);
        if (count <= 0)
            return out;

        double arcR = mainRingOuterR + 26.0 + itemSizeDip * 0.5;
        double step = (itemSizeDip + clearanceDip) / arcR;

        if (count > 1)
        {
            for (int iter = 0; iter < 3; ++iter)
            {
                step = (itemSizeDip + clearanceDip) / arcR;
                const double span = static_cast<double>(count - 1) * step;
                if (span <= maxSpanRad)
                    break;
                arcR = (std::min)(arcR * (span / maxSpanRad), mainRingOuterR + 120.0);
            }
            const double maxAllowedStep = maxSpanRad / static_cast<double>(count - 1);
            step = (std::min)(step, maxAllowedStep);
        }

        out.arcRadius = arcR;
        out.trackWidthDip = itemSizeDip + 16.0; // 40 + 16 = 56 DIP wide continuous arc track
        out.angles.reserve(static_cast<size_t>(count));
        out.rects.reserve(static_cast<size_t>(count));

        double minDelta = 0.0;
        double maxDelta = 0.0;
        for (int i = 0; i < count; ++i)
        {
            double delta = 0.0;
            if (alternatingFromCenter)
            {
                const int offset = (i == 0) ? 0 : ((i + 1) / 2) * ((i % 2 == 1) ? -1 : 1);
                delta = static_cast<double>(offset) * step;
            }
            else
            {
                delta = (static_cast<double>(i) - static_cast<double>(count - 1) * 0.5) * step;
            }
            if (i == 0 || delta < minDelta) minDelta = delta;
            if (i == 0 || delta > maxDelta) maxDelta = delta;

            const double a = baseAngleRad + delta;
            const double cx = centerX + arcR * std::cos(a);
            const double cy = centerY + arcR * std::sin(a);
            out.angles.push_back(a);
            out.rects.push_back(RectD{
                cx - itemSizeDip * 0.5,
                cy - itemSizeDip * 0.5,
                itemSizeDip,
                itemSizeDip
            });
        }
        out.startAngleRad = baseAngleRad + minDelta;
        out.endAngleRad = baseAngleRad + maxDelta;
        return out;
    }

    inline std::vector<RectD> ComputeFolderSubmenuRects(
        double centerX,
        double centerY,
        double baseAngleRad,
        double ringRadiusDip,
        double iconSizeDip,
        int itemCount,
        double badgeSizeDip = 40.0,
        bool alternatingFromCenter = false)
    {
        const double mainOuterR = MainRingOuterRadius(ringRadiusDip, iconSizeDip);
        return ComputeIconArcLayout(
            centerX, centerY, baseAngleRad, mainOuterR,
            itemCount, badgeSizeDip, 12.0, 4.01426, alternatingFromCenter).rects;
    }

    inline std::vector<RectD> ComputeWindowOuterArcRects(
        double centerX,
        double centerY,
        double baseAngleRad,
        double ringRadiusDip,
        double iconSizeDip,
        double thumbnailScale,
        int itemCount,
        double angularStepRad = 0.36)
    {
        const int count = std::clamp(itemCount, 0, 9);
        const double cardW = 104.0 * thumbnailScale;
        const double cardH = 68.0 * thumbnailScale;
        const double cardHalfDiag = 0.5 * std::hypot(cardW, cardH);
        const double mainOuterR = MainRingOuterRadius(ringRadiusDip, iconSizeDip);
        const double outerR = (std::max)(mainOuterR + 26.0 + 0.5 * (std::max)(cardW, cardH),
                                         mainOuterR + 8.0 + cardHalfDiag);
        const double neededStep = (cardW * 0.92 + 10.0) / outerR;
        const double step = (std::clamp)((std::max)(angularStepRad, neededStep), 0.24, 0.52);
        const auto centers = GetOuterArcCenters(centerX, centerY, baseAngleRad, outerR, count, step);
        std::vector<RectD> rects;
        rects.reserve(centers.size());
        for (const auto& c : centers)
        {
            rects.push_back(RectD{
                c.x - cardW * 0.5,
                c.y - cardH * 0.5,
                cardW,
                cardH
            });
        }
        return rects;
    }

    inline std::vector<RectD> ComputeTitleColumnRects(
        double centerX,
        double centerY,
        double overlaySizeDip,
        double baseAngleRad,
        double ringRadiusDip,
        double iconSizeDip,
        int itemCount)
    {
        const int count = std::clamp(itemCount, 0, 9);
        std::vector<RectD> rects;
        if (count <= 0)
            return rects;

        constexpr double kMargin = 10.0;
        constexpr double kRowH = 30.0;
        constexpr double kRowGap = 4.0;
        const double totalH = count * kRowH + (count - 1) * kRowGap;
        const double ringClearR = MainRingOuterRadius(ringRadiusDip, iconSizeDip) + 10.0;
        const double availSide = (std::min)(centerX, overlaySizeDip - centerX) - ringClearR - kMargin;
        const double rowW = std::clamp(availSide, 130.0, 190.0);

        const double cosA = std::cos(baseAngleRad);
        const double sinA = std::sin(baseAngleRad);
        const double anchorX = centerX + ringRadiusDip * cosA;
        const double anchorY = centerY + ringRadiusDip * sinA;

        double startX = 0.0;
        double startY = 0.0;

        if (std::abs(cosA) >= std::abs(sinA))
        {
            startX = (cosA >= 0.0)
                ? std::clamp(centerX + ringClearR, kMargin, overlaySizeDip - rowW - kMargin)
                : std::clamp(centerX - ringClearR - rowW, kMargin, overlaySizeDip - rowW - kMargin);
            startY = std::clamp(anchorY - totalH * 0.5, kMargin, overlaySizeDip - totalH - kMargin);
        }
        else
        {
            const double availV = ((sinA < 0.0) ? centerY : (overlaySizeDip - centerY)) - ringClearR - kMargin;
            if (totalH <= availV)
            {
                startX = std::clamp(anchorX - rowW * 0.5, kMargin, overlaySizeDip - rowW - kMargin);
                startY = (sinA < 0.0)
                    ? (centerY - ringClearR - totalH)
                    : (centerY + ringClearR);
            }
            else
            {
                startY = (sinA < 0.0) ? kMargin : (overlaySizeDip - totalH - kMargin);
                double minAbsDy = 0.0;
                if (centerY < startY)
                    minAbsDy = startY - centerY;
                else if (centerY > startY + totalH)
                    minAbsDy = centerY - (startY + totalH);

                const double clearDx = (minAbsDy < ringClearR)
                    ? std::sqrt(ringClearR * ringClearR - minAbsDy * minAbsDy)
                    : 0.0;
                startX = (cosA >= 0.0)
                    ? std::clamp(centerX + clearDx, kMargin, overlaySizeDip - rowW - kMargin)
                    : std::clamp(centerX - clearDx - rowW, kMargin, overlaySizeDip - rowW - kMargin);
            }
        }

        rects.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            rects.push_back(RectD{ startX, startY + i * (kRowH + kRowGap), rowW, kRowH });
        return rects;
    }

    inline RectD ComputeUnionBounds(const std::vector<RectD>& rects) noexcept
    {
        if (rects.empty())
            return {};
        double minL = 1e9, minT = 1e9, maxR = -1e9, maxB = -1e9;
        for (const auto& r : rects)
        {
            if (r.IsEmpty())
                continue;
            minL = (std::min)(minL, r.left);
            minT = (std::min)(minT, r.top);
            maxR = (std::max)(maxR, r.Right());
            maxB = (std::max)(maxB, r.Bottom());
        }
        if (maxR <= minL || maxB <= minT)
            return {};
        return RectD{ minL, minT, maxR - minL, maxB - minT };
    }

    inline int HitTestRects(PointD pt, const std::vector<RectD>& rects) noexcept
    {
        for (size_t i = 0; i < rects.size(); ++i)
        {
            if (rects[i].Contains(pt))
                return static_cast<int>(i);
        }
        return -1;
    }

    inline bool IsPointInHoverCorridor(PointD cursor, PointD origin, RectD targetBounds, double padding = 18.0) noexcept
    {
        if (targetBounds.IsEmpty())
            return false;

        const RectD expanded{
            targetBounds.left - padding,
            targetBounds.top - padding,
            targetBounds.width + padding * 2.0,
            targetBounds.height + padding * 2.0
        };

        if (expanded.Contains(cursor))
            return true;

        // If origin lies inside the expanded target rect, never project a wraparound cone.
        if (expanded.Contains(origin))
            return false;

        // Exact convex-hull test for Conv({origin} U expanded):
        // cursor lies on a segment from origin to some point in expanded iff the ray
        // Q(t) = origin + t * (cursor - origin) intersects expanded for some t >= 1.0.
        const double dx = cursor.x - origin.x;
        const double dy = cursor.y - origin.y;
        const bool zeroDx = (dx >= -1e-9 && dx <= 1e-9);
        const bool zeroDy = (dy >= -1e-9 && dy <= 1e-9);
        if (zeroDx && zeroDy)
            return false;

        double tMin = 1.0;
        double tMax = 1e30;

        if (zeroDx)
        {
            if (origin.x < expanded.left || origin.x > expanded.Right())
                return false;
        }
        else
        {
            double tx1 = (expanded.left - origin.x) / dx;
            double tx2 = (expanded.Right() - origin.x) / dx;
            if (tx1 > tx2)
                std::swap(tx1, tx2);
            tMin = (std::max)(tMin, tx1);
            tMax = (std::min)(tMax, tx2);
        }

        if (zeroDy)
        {
            if (origin.y < expanded.top || origin.y > expanded.Bottom())
                return false;
        }
        else
        {
            double ty1 = (expanded.top - origin.y) / dy;
            double ty2 = (expanded.Bottom() - origin.y) / dy;
            if (ty1 > ty2)
                std::swap(ty1, ty2);
            tMin = (std::max)(tMin, ty1);
            tMax = (std::min)(tMax, ty2);
        }

        return tMax >= tMin;
    }

    inline bool IsPointInOuterSubmenuZone(
        PointD cursor,
        PointD center,
        PointD corridorOrigin,
        double baseAngleRad,
        double mainRingOuterRadius,
        const std::vector<RectD>& cardRects,
        RectD submenuBounds,
        bool isTitleColumn) noexcept
    {
        if (cardRects.empty() || submenuBounds.IsEmpty())
            return false;

        const double dx = cursor.x - center.x;
        const double dy = cursor.y - center.y;
        const double distFromCenter = std::hypot(dx, dy);

        // Never lock a submenu open inside the main ring (other slot icons/sectors or deadzone).
        if (distFromCenter <= mainRingOuterRadius)
            return false;

        if (isTitleColumn)
            return IsPointInHoverCorridor(cursor, corridorOrigin, submenuBounds, 10.0);

        // OuterArc submenu (Folder / Collection / multi-window OuterArc):
        // 1. Tight annular arc band hugging the actual outer cards (not their hollow AABB).
        double minDelta = 1e9;
        double maxDelta = -1e9;
        double minCardInnerR = 1e9;
        double maxCardOuterR = 0.0;

        for (const auto& r : cardRects)
        {
            if (r.IsEmpty())
                continue;
            const double cx = r.left + r.width * 0.5;
            const double cy = r.top + r.height * 0.5;
            const double cdx = cx - center.x;
            const double cdy = cy - center.y;
            const double cardR = std::hypot(cdx, cdy);
            const double cardHalfDiag = 0.5 * std::hypot(r.width, r.height);
            const double cardHalfRadial = (cardR > 1e-6)
                ? (0.5 * (r.width * std::abs(cdx / cardR) + r.height * std::abs(cdy / cardR)))
                : (0.5 * (std::max)(r.width, r.height));

            minCardInnerR = (std::min)(minCardInnerR, (std::max)(mainRingOuterRadius, cardR - cardHalfRadial));
            maxCardOuterR = (std::max)(maxCardOuterR, cardR + cardHalfDiag);

            const double cardAngle = std::atan2(cdy, cdx);
            const double delta = NormalizeAngleDelta(cardAngle - baseAngleRad);
            const double halfAng = (cardR > 1.0) ? std::atan2(cardHalfDiag, cardR) : 0.2;
            minDelta = (std::min)(minDelta, delta - halfAng);
            maxDelta = (std::max)(maxDelta, delta + halfAng);
        }

        if (minDelta > maxDelta)
            return false;

        const double cursorDelta = NormalizeAngleDelta(std::atan2(dy, dx) - baseAngleRad);
        const double bandInnerR = (std::max)(mainRingOuterRadius, minCardInnerR - 6.0);
        const double bandOuterR = maxCardOuterR + 12.0;

        if (distFromCenter >= bandInnerR && distFromCenter <= bandOuterR &&
            cursorDelta >= minDelta - 0.05 && cursorDelta <= maxDelta + 0.05)
        {
            return true;
        }

        // 2. Narrow bridge strictly outside the main ring from corridorOrigin toward individual cards.
        if (distFromCenter <= bandOuterR)
        {
            for (const auto& r : cardRects)
            {
                if (IsPointInHoverCorridor(cursor, corridorOrigin, r, 6.0))
                    return true;
            }
        }

        return false;
    }

    struct RingHoverResult
    {
        int selectedSlot = -1;
        int selectedSubIndex = -1;
        bool keepExistingSubmenu = false;
        bool updateCorridorOrigin = false;
    };

    inline RingHoverResult ResolveRingHover(
        PointD cursor,
        PointD center,
        int slotCount,
        double deadzoneDip,
        double ringRadiusDip,
        double iconSizeDip,
        int currentSelectedSlot,
        int expandedSlotIndex,
        PointD corridorOrigin,
        const std::vector<RectD>& submenuCardRects,
        RectD submenuBounds,
        bool submenuIsTitleColumn,
        double mainRingOuterReachDip = 12.0) noexcept
    {
        RingHoverResult res{};
        if (slotCount <= 0)
            return res;

        const double dx = cursor.x - center.x;
        const double dy = cursor.y - center.y;
        const double distFromCenter = std::hypot(dx, dy);

        // 1. Center deadzone always deselects and collapses any open submenu (except search mode handled by caller).
        if (distFromCenter <= deadzoneDip)
            return res;

        const double slotBadgeRadius = (iconSizeDip + 14.0) * 0.5;
        const double mainRingOuterR = MainRingOuterRadius(ringRadiusDip, iconSizeDip);

        // 2. Direct hover over any OTHER main-ring slot icon immediately switches away,
        //    even if a submenu or top search half-ring is currently open.
        if ((expandedSlotIndex >= 0 && expandedSlotIndex < slotCount) || expandedSlotIndex == -2)
        {
            int directOtherSlot = -1;
            double bestDist = slotBadgeRadius;
            for (int i = 0; i < slotCount; ++i)
            {
                if (i == expandedSlotIndex)
                    continue;
                const PointD sc = GetSlotCenter(center.x, center.y, ringRadiusDip, i, slotCount);
                const double d = std::hypot(cursor.x - sc.x, cursor.y - sc.y);
                if (d <= bestDist)
                {
                    bestDist = d;
                    directOtherSlot = i;
                }
            }
            if (directOtherSlot >= 0)
            {
                res.selectedSlot = directOtherSlot;
                res.selectedSubIndex = -1;
                res.keepExistingSubmenu = (expandedSlotIndex == -2);
                return res;
            }
        }

        // 3. Submenu / Top-Search-Arc card hit and outer submenu zone/corridor (strictly outside mainRingOuterR).
        if (((expandedSlotIndex >= 0 && expandedSlotIndex < slotCount) || expandedSlotIndex == -2) &&
            !submenuCardRects.empty())
        {
            const int hitSub = HitTestRects(cursor, submenuCardRects);
            if (hitSub >= 0)
            {
                res.selectedSlot = expandedSlotIndex;
                res.selectedSubIndex = hitSub;
                res.keepExistingSubmenu = true;
                return res;
            }

            const double expandedAngle = (expandedSlotIndex == -2)
                ? (-kPi * 0.5)
                : GetSlotAngle(expandedSlotIndex, slotCount);
            if (IsPointInOuterSubmenuZone(
                cursor,
                center,
                corridorOrigin,
                expandedAngle,
                mainRingOuterR,
                submenuCardRects,
                submenuBounds,
                submenuIsTitleColumn))
            {
                res.selectedSlot = expandedSlotIndex;
                res.selectedSubIndex = -1;
                res.keepExistingSubmenu = true;
                return res;
            }
        }

        // 4. Outer boundary cap: hovering far outside the ring MUST NOT select unhovered apps
        //    or summon their invisible PiPs. Only [deadzoneDip, mainRingOuterR + mainRingOuterReachDip]
        //    selects a main-ring slot via its angular cone.
        if (distFromCenter > mainRingOuterR + (std::max)(0.0, mainRingOuterReachDip))
            return res;

        const int nextSlot = CalculateSectorIndex(dx, dy, slotCount, deadzoneDip);
        res.selectedSlot = nextSlot;
        res.selectedSubIndex = -1;
        res.keepExistingSubmenu = (nextSlot >= 0 && nextSlot == expandedSlotIndex) || (expandedSlotIndex == -2);
        res.updateCorridorOrigin = (nextSlot >= 0 && nextSlot == currentSelectedSlot && distFromCenter <= mainRingOuterR);
        return res;
    }

    inline double EaseOutCubic(double t) noexcept
    {
        const double clamped = std::clamp(t, 0.0, 1.0);
        const double inv = 1.0 - clamped;
        return 1.0 - inv * inv * inv;
    }

    inline PointD ClampRingCenter(double cursorX, double cursorY, double halfOverlaySize, RectD workArea) noexcept
    {
        const double minX = workArea.left + halfOverlaySize;
        const double maxX = workArea.Right() - halfOverlaySize;
        const double minY = workArea.top + halfOverlaySize;
        const double maxY = workArea.Bottom() - halfOverlaySize;

        const double clampedX = (minX <= maxX)
            ? std::clamp(cursorX, minX, maxX)
            : workArea.left + workArea.width * 0.5;
        const double clampedY = (minY <= maxY)
            ? std::clamp(cursorY, minY, maxY)
            : workArea.top + workArea.height * 0.5;
        return { clampedX, clampedY };
    }

    inline int StepIndex(int currentIndex, int count, int wheelDelta) noexcept
    {
        if (count <= 0)
            return -1;
        const int step = (wheelDelta < 0) ? 1 : -1;
        if (currentIndex < 0)
            return (step > 0) ? 0 : (count - 1);
        return (currentIndex + step + count) % count;
    }

    enum class SnapZone
    {
        None = 0,
        Left,
        Right,
        TopMaximize
    };

    inline SnapZone ClassifySnapEdge(
        int ptX, int ptY,
        int monLeft, int monTop, int monRight, int monBottom,
        int sideMarginPx, int topMarginPx) noexcept
    {
        if (ptX < monLeft || ptX >= monRight || ptY < monTop || ptY >= monBottom)
            return SnapZone::None;
        const int side = (std::max)(8, sideMarginPx);
        const int top = (std::max)(8, topMarginPx);
        if (ptX <= monLeft + side)
            return SnapZone::Left;
        if (ptX >= monRight - side)
            return SnapZone::Right;
        if (ptY <= monTop + top)
            return SnapZone::TopMaximize;
        return SnapZone::None;
    }
} // namespace winrings
