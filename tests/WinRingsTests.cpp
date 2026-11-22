#include <Windows.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/shared/ClockFormat.h"
#include "../src/shared/CrashGuard.h"
#include "../src/shared/RadialMath.h"
#include "../src/shared/RingThemeEngine.h"
#include "../src/shared/SearchMatch.h"
#include "../src/shared/SlotMatching.h"
#include "../src/shared/WinRingsConfig.h"

using namespace winrings;

static int g_failures = 0;

#define CHECK(cond)                                                                      \
    do                                                                                   \
    {                                                                                    \
        if (!(cond))                                                                     \
        {                                                                                \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

static void TestRadialMath()
{
    CHECK(CalculateSectorIndex(10.0, -10.0, 6, 18.0) == -1);
    CHECK(CalculateSectorIndex(0.0, -24.0, 8, 18.0) == 0);
    CHECK(CalculateSectorIndex(0.0, -80.0, 4, 18.0) == 0);
    CHECK(CalculateSectorIndex(80.0, 0.0, 4, 18.0) == 1);
    CHECK(CalculateSectorIndex(0.0, 80.0, 4, 18.0) == 2);
    CHECK(CalculateSectorIndex(-80.0, 0.0, 4, 18.0) == 3);
    CHECK(CalculateSectorIndex(100.0, 0.0, 0, 18.0) == -1);

    CHECK(StepIndex(-1, 6, -120) == 0);
    CHECK(StepIndex(-1, 6, 120) == 5);
    CHECK(StepIndex(5, 6, -120) == 0);
    CHECK(StepIndex(0, 6, 120) == 5);

    const auto arc = GetOuterArcCenters(340.0, 340.0, -kPi / 2.0, 200.0, 14);
    CHECK(arc.size() == 9);
    CHECK(std::abs(arc[0].x - 340.0) < 0.1);
    CHECK(std::abs(arc[0].y - 140.0) < 0.1);

    const PointD origin{ 340.0, 260.0 };
    const RectD submenu{ 280.0, 80.0, 120.0, 80.0 };
    CHECK(IsPointInHoverCorridor({ 340.0, 200.0 }, origin, submenu));
    CHECK(IsPointInHoverCorridor({ 320.0, 110.0 }, origin, submenu));
    CHECK(!IsPointInHoverCorridor({ 340.0, 360.0 }, origin, submenu));
    // Must not overshoot far past the submenu target rect.
    CHECK(!IsPointInHoverCorridor({ 340.0, 10.0 }, origin, submenu));
    // When origin is inside the expanded rect, must not project a 360-degree wraparound cone outside it.
    CHECK(!IsPointInHoverCorridor({ 340.0, 220.0 }, { 340.0, 120.0 }, submenu));
    CHECK(!IsPointInHoverCorridor({ 100.0, 120.0 }, { 340.0, 120.0 }, submenu));

    const RectD workArea{ 0.0, 30.0, 1920.0, 1050.0 };
    const auto clamped = ClampRingCenter(20.0, 10.0, 160.0, workArea);
    CHECK(std::abs(clamped.x - 160.0) < 0.01);
    CHECK(std::abs(clamped.y - 190.0) < 0.01);

    CHECK(std::abs(EaseOutCubic(0.0) - 0.0) < 1e-6);
    CHECK(std::abs(EaseOutCubic(1.0) - 1.0) < 1e-6);
    CHECK(EaseOutCubic(0.5) > 0.5);
}

static bool RectIntersectsCircle(RectD r, PointD c, double radius) noexcept
{
    const double closestX = std::clamp(c.x, r.left, r.Right());
    const double closestY = std::clamp(c.y, r.top, r.Bottom());
    return std::hypot(closestX - c.x, closestY - c.y) < radius;
}

static void TestDownloadsAndSubmenuHoverTransitions()
{
    constexpr double kOverlay = 680.0;
    constexpr PointD kCenter{ 340.0, 340.0 };
    constexpr double kDeadzone = 18.0;
    constexpr double kRingR = 112.0;
    constexpr double kIconSize = 42.0;
    const double mainRingOuterR = MainRingOuterRadius(kRingR, kIconSize);
    CHECK(std::abs(mainRingOuterR - 145.0) < 0.01);

    const RingGeometry a4sGeo = ComputeRingGeometry(8, 115.0, 44.0, 18.0);
    CHECK(std::abs(a4sGeo.ringRadius - 115.0) < 0.01);
    CHECK(std::abs(a4sGeo.outerRadius - 149.0) < 0.01);
    CHECK(a4sGeo.holeRadius > 18.0 && a4sGeo.holeRadius < a4sGeo.bandRadius);

    const IconArcLayout topSearchArc = ComputeIconArcLayout(kCenter.x, kCenter.y, -kPi * 0.5, a4sGeo.outerRadius, 6);
    CHECK(topSearchArc.rects.size() == 6);
    CHECK(std::abs(topSearchArc.trackWidthDip - 56.0) < 0.01);
    CHECK(std::abs((topSearchArc.startAngleRad + topSearchArc.endAngleRad) * 0.5 - (-kPi * 0.5)) < 0.01);

    // 1. Exact Bug 1 reproduction: Downloads (Folder, slot 1) with 8 items in 3-, 6-, 8-, and 14-slot rings.
    const int ringSizes[] = { 3, 6, 8, 14 };
    for (int slotCount : ringSizes)
    {
        const int downloadsSlot = 1;
        const double dlAngle = GetSlotAngle(downloadsSlot, slotCount);
        const auto cards = ComputeFolderSubmenuRects(kCenter.x, kCenter.y, dlAngle, kRingR, kIconSize, 8);
        CHECK(cards.size() == 8);
        const RectD bounds = ComputeUnionBounds(cards);

        // No submenu card may intersect the main ring.
        for (const auto& card : cards)
            CHECK(!RectIntersectsCircle(card, kCenter, mainRingOuterR));

        // Test transitions starting both from barely outside deadzone (r=22) AND from the Downloads icon (r=112).
        const PointD origins[2] = {
            { kCenter.x + 22.0 * std::cos(dlAngle), kCenter.y + 22.0 * std::sin(dlAngle) },
            GetSlotCenter(kCenter.x, kCenter.y, kRingR, downloadsSlot, slotCount)
        };

        for (const PointD& origin : origins)
        {
            // Moving to EVERY other main-ring slot icon (and inner/outer ring points in its sector) MUST switch immediately.
            for (int other = 0; other < slotCount; ++other)
            {
                if (other == downloadsSlot)
                    continue;
                const double otherAngle = GetSlotAngle(other, slotCount);
                const PointD testPts[3] = {
                    GetSlotCenter(kCenter.x, kCenter.y, kRingR, other, slotCount),
                    { kCenter.x + 55.0 * std::cos(otherAngle), kCenter.y + 55.0 * std::sin(otherAngle) },
                    { kCenter.x + (mainRingOuterR - 2.0) * std::cos(otherAngle), kCenter.y + (mainRingOuterR - 2.0) * std::sin(otherAngle) }
                };
                for (const PointD& pt : testPts)
                {
                    const auto res = ResolveRingHover(
                        pt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                        downloadsSlot, downloadsSlot, origin, cards, bounds, false);
                    CHECK(res.selectedSlot == other);
                    CHECK(res.selectedSubIndex == -1);
                    CHECK(!res.keepExistingSubmenu);
                }
            }

            // Moving into center deadzone MUST immediately deselect and collapse submenu.
            const PointD deadzonePts[4] = {
                kCenter,
                { kCenter.x + 10.0, kCenter.y - 6.0 },
                { kCenter.x, kCenter.y - kDeadzone },
                { kCenter.x + (kDeadzone - 0.5) * std::cos(dlAngle), kCenter.y + (kDeadzone - 0.5) * std::sin(dlAngle) }
            };
            for (const PointD& pt : deadzonePts)
            {
                const auto res = ResolveRingHover(
                    pt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                    downloadsSlot, downloadsSlot, origin, cards, bounds, false);
                CHECK(res.selectedSlot == -1);
                CHECK(res.selectedSubIndex == -1);
                CHECK(!res.keepExistingSubmenu);
            }
        }

        // Gliding straight out from Downloads icon across the gap into card 0 keeps submenu open and selects card 0.
        const PointD dlCenter = GetSlotCenter(kCenter.x, kCenter.y, kRingR, downloadsSlot, slotCount);
        const PointD bridgePt{ kCenter.x + 165.0 * std::cos(dlAngle), kCenter.y + 165.0 * std::sin(dlAngle) };
        const auto bridgeRes = ResolveRingHover(
            bridgePt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
        CHECK(bridgeRes.selectedSlot == downloadsSlot);
        CHECK(bridgeRes.selectedSubIndex == -1);
        CHECK(bridgeRes.keepExistingSubmenu);

        // Hovering every submenu card selects that exact card; gliding in inter-card gaps keeps submenu open.
        for (size_t i = 0; i < cards.size(); ++i)
        {
            const PointD cardCenter{ cards[i].left + cards[i].width * 0.5, cards[i].top + cards[i].height * 0.5 };
            const auto cardRes = ResolveRingHover(
                cardCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
            CHECK(cardRes.selectedSlot == downloadsSlot);
            CHECK(cardRes.selectedSubIndex == static_cast<int>(i));
            CHECK(cardRes.keepExistingSubmenu);
        }

        // Gap between adjacent items along the continuous half-ring track keeps Downloads open.
        const auto dlArc = ComputeIconArcLayout(kCenter.x, kCenter.y, dlAngle, mainRingOuterR, 8);
        CHECK(dlArc.angles.size() == 8);
        const double gapAngle = 0.5 * (dlArc.angles[0] + dlArc.angles[1]);
        const PointD gapPt{ kCenter.x + dlArc.arcRadius * std::cos(gapAngle), kCenter.y + dlArc.arcRadius * std::sin(gapAngle) };
        const auto gapRes = ResolveRingHover(
            gapPt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
        CHECK(gapRes.selectedSlot == downloadsSlot);
        CHECK(gapRes.selectedSubIndex == -1);
        CHECK(gapRes.keepExistingSubmenu);

        // Moving back onto Downloads icon clears stale sub-selection while keeping Downloads open & updating origin.
        const auto backRes = ResolveRingHover(
            dlCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, origins[0], cards, bounds, false);
        CHECK(backRes.selectedSlot == downloadsSlot);
        CHECK(backRes.selectedSubIndex == -1);
        CHECK(backRes.keepExistingSubmenu);
        CHECK(backRes.updateCorridorOrigin);

        // Moving far outside the opposite side of the ring MUST NOT select an unhovered icon / invisible PiP.
        const int oppositeSlot = (downloadsSlot + slotCount / 2) % slotCount;
        const double oppAngle = GetSlotAngle(oppositeSlot, slotCount);
        const PointD farOpp{ kCenter.x + 230.0 * std::cos(oppAngle), kCenter.y + 230.0 * std::sin(oppAngle) };
        const auto oppRes = ResolveRingHover(
            farOpp, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false);
        CHECK(oppRes.selectedSlot == -1);
        CHECK(oppRes.selectedSubIndex == -1);
        CHECK(!oppRes.keepExistingSubmenu);

        // Bounded outer reach: sticky (12 DIP) rejects r = mainRingOuterR + 40, while holding reach (48 DIP) selects oppositeSlot.
        const PointD midOutOpp{
            kCenter.x + (mainRingOuterR + 40.0) * std::cos(oppAngle),
            kCenter.y + (mainRingOuterR + 40.0) * std::sin(oppAngle)
        };
        const auto stickyFarRes = ResolveRingHover(
            midOutOpp, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false, 12.0);
        CHECK(stickyFarRes.selectedSlot == -1);
        const auto holdFarRes = ResolveRingHover(
            midOutOpp, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            downloadsSlot, downloadsSlot, dlCenter, cards, bounds, false, 48.0);
        CHECK(holdFarRes.selectedSlot == oppositeSlot);
        CHECK(!holdFarRes.keepExistingSubmenu);
    }

    // 2. Multi-window App in OuterArc mode across thumbnailScale = 0.5, 1.0, 1.5 (up to 9 windows).
    const double scales[] = { 0.5, 1.0, 1.5 };
    for (double scale : scales)
    {
        const int slotCount = 8;
        const int appSlot = 2;
        const double baseAngle = GetSlotAngle(appSlot, slotCount);
        const auto cards = ComputeWindowOuterArcRects(kCenter.x, kCenter.y, baseAngle, kRingR, kIconSize, scale, 9);
        CHECK(cards.size() == 9);
        const RectD bounds = ComputeUnionBounds(cards);
        const PointD origin = GetSlotCenter(kCenter.x, kCenter.y, kRingR, appSlot, slotCount);

        for (const auto& card : cards)
            CHECK(!RectIntersectsCircle(card, kCenter, mainRingOuterR));

        // Direct hit on outermost window card (index 8) works.
        const PointD lastCard{ cards[8].left + cards[8].width * 0.5, cards[8].top + cards[8].height * 0.5 };
        const auto subRes = ResolveRingHover(
            lastCard, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
            appSlot, appSlot, origin, cards, bounds, false);
        CHECK(subRes.selectedSlot == appSlot);
        CHECK(subRes.selectedSubIndex == 8);
        CHECK(subRes.keepExistingSubmenu);

        // Moving onto ANY other main-ring slot icon immediately switches away.
        for (int other = 0; other < slotCount; ++other)
        {
            if (other == appSlot)
                continue;
            const PointD otherPt = GetSlotCenter(kCenter.x, kCenter.y, kRingR, other, slotCount);
            const auto res = ResolveRingHover(
                otherPt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                appSlot, appSlot, origin, cards, bounds, false);
            CHECK(res.selectedSlot == other);
            CHECK(!res.keepExistingSubmenu);
        }
    }

    // 3. Multi-window App in TitleColumn mode (1, 5, 9 windows) at all 8 compass slots.
    const int counts[] = { 1, 5, 9 };
    for (int count : counts)
    {
        const int slotCount = 8;
        for (int appSlot = 0; appSlot < slotCount; ++appSlot)
        {
            const double baseAngle = GetSlotAngle(appSlot, slotCount);
            const auto cards = ComputeTitleColumnRects(kCenter.x, kCenter.y, kOverlay, baseAngle, kRingR, kIconSize, count);
            CHECK(static_cast<int>(cards.size()) == count);
            const RectD bounds = ComputeUnionBounds(cards);
            const PointD origin = GetSlotCenter(kCenter.x, kCenter.y, kRingR, appSlot, slotCount);

            // TitleColumn cards must stay inside overlay and must NEVER overlap the main ring.
            for (const auto& card : cards)
            {
                CHECK(card.left >= 0.0 && card.Right() <= kOverlay);
                CHECK(card.top >= 0.0 && card.Bottom() <= kOverlay);
                CHECK(!RectIntersectsCircle(card, kCenter, mainRingOuterR));
            }

            // Hovering every row selects that exact row.
            for (int r = 0; r < count; ++r)
            {
                const PointD rowCenter{ cards[static_cast<size_t>(r)].left + cards[static_cast<size_t>(r)].width * 0.5,
                                        cards[static_cast<size_t>(r)].top + cards[static_cast<size_t>(r)].height * 0.5 };
                const auto rowRes = ResolveRingHover(
                    rowCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                    appSlot, appSlot, origin, cards, bounds, true);
                CHECK(rowRes.selectedSlot == appSlot);
                CHECK(rowRes.selectedSubIndex == r);
                CHECK(rowRes.keepExistingSubmenu);
            }

            // Gliding strictly outside the main ring straight from origin toward TitleColumn center keeps it open.
            const PointD colCenter{ bounds.left + bounds.width * 0.5, bounds.top + bounds.height * 0.5 };
            const double dirX = colCenter.x - origin.x;
            const double dirY = colCenter.y - origin.y;
            const double dirLen = std::hypot(dirX, dirY);
            if (dirLen > 1.0)
            {
                // Step along segment origin -> colCenter until outside mainRingOuterR + 4 and before entering bounds.
                for (double t = 0.1; t < 0.95; t += 0.05)
                {
                    const PointD p{ origin.x + t * dirX, origin.y + t * dirY };
                    if (std::hypot(p.x - kCenter.x, p.y - kCenter.y) > mainRingOuterR + 4.0 && !bounds.Contains(p))
                    {
                        const auto corrRes = ResolveRingHover(
                            p, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                            appSlot, appSlot, origin, cards, bounds, true);
                        CHECK(corrRes.selectedSlot == appSlot);
                        CHECK(corrRes.keepExistingSubmenu);
                        break;
                    }
                }
            }

            // Moving to ANY other main-ring slot icon or center deadzone immediately collapses TitleColumn.
            for (int other = 0; other < slotCount; ++other)
            {
                if (other == appSlot)
                    continue;
                const PointD otherPt = GetSlotCenter(kCenter.x, kCenter.y, kRingR, other, slotCount);
                const auto res = ResolveRingHover(
                    otherPt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                    appSlot, appSlot, origin, cards, bounds, true);
                CHECK(res.selectedSlot == other);
                CHECK(!res.keepExistingSubmenu);
            }

            const auto deadRes = ResolveRingHover(
                kCenter, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                appSlot, appSlot, origin, cards, bounds, true);
            CHECK(deadRes.selectedSlot == -1);
            CHECK(!deadRes.keepExistingSubmenu);
        }
    }

    // 4. End-to-end multi-step trajectory simulating RingOverlay::UpdatePointer with Downloads + other icons.
    {
        constexpr int slotCount = 6;
        constexpr int downloadsSlot = 1; // -30 deg (2 o'clock)
        constexpr int explorerSlot = 0;  // -90 deg (12 o'clock)
        constexpr int recycleSlot = 2;   // +30 deg (4 o'clock)
        constexpr int collectionSlot = 4; // +150 deg (8 o'clock)

        int selectedSlot = -1;
        int expandedSlot = -1;
        int selectedSub = -1;
        PointD corridorOrigin = kCenter;
        std::vector<RectD> subCards;
        RectD subBounds{};

        auto stepPointer = [&](PointD pt) {
            const auto res = ResolveRingHover(
                pt, kCenter, slotCount, kDeadzone, kRingR, kIconSize,
                selectedSlot, expandedSlot, corridorOrigin, subCards, subBounds, false);
            if (res.selectedSlot != selectedSlot)
            {
                selectedSlot = res.selectedSlot;
                selectedSub = -1;
                if (selectedSlot == downloadsSlot || selectedSlot == collectionSlot)
                {
                    expandedSlot = selectedSlot;
                    corridorOrigin = pt;
                    const int itemCnt = (selectedSlot == downloadsSlot) ? 8 : 5;
                    subCards = ComputeFolderSubmenuRects(
                        kCenter.x, kCenter.y, GetSlotAngle(selectedSlot, slotCount), kRingR, kIconSize, itemCnt);
                    subBounds = ComputeUnionBounds(subCards);
                }
                else
                {
                    expandedSlot = -1;
                    subCards.clear();
                    subBounds = {};
                }
            }
            else
            {
                if (!res.keepExistingSubmenu)
                {
                    expandedSlot = -1;
                    subCards.clear();
                    subBounds = {};
                }
                if (res.updateCorridorOrigin)
                    corridorOrigin = pt;
                selectedSub = res.selectedSubIndex;
            }
        };

        // Enter Downloads just outside deadzone -> Downloads expands!
        const double dlAngle = GetSlotAngle(downloadsSlot, slotCount);
        stepPointer({ kCenter.x + 24.0 * std::cos(dlAngle), kCenter.y + 24.0 * std::sin(dlAngle) });
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == -1);
        // Verify that the naive AABB of Downloads' 8-item outer arc actually encloses Explorer and center!
        CHECK(subBounds.Contains(GetSlotCenter(kCenter.x, kCenter.y, kRingR, explorerSlot, slotCount)) ||
              IsPointInHoverCorridor(GetSlotCenter(kCenter.x, kCenter.y, kRingR, explorerSlot, slotCount), corridorOrigin, subBounds));

        // Move out to Downloads icon -> origin updates, still expanded.
        const PointD dlIcon = GetSlotCenter(kCenter.x, kCenter.y, kRingR, downloadsSlot, slotCount);
        stepPointer(dlIcon);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot);
        CHECK(std::abs(corridorOrigin.x - dlIcon.x) < 0.01 && std::abs(corridorOrigin.y - dlIcon.y) < 0.01);

        // Move into Downloads outer-arc card #0, then glide via gap to card #5.
        const PointD card0{ subCards[0].left + subCards[0].width * 0.5, subCards[0].top + subCards[0].height * 0.5 };
        stepPointer(card0);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == 0);

        const auto simArc = ComputeIconArcLayout(kCenter.x, kCenter.y, dlAngle, mainRingOuterR, 8);
        const double simGapAngle = 0.5 * (simArc.angles[4] + simArc.angles[5]);
        const PointD card5{ subCards[5].left + subCards[5].width * 0.5, subCards[5].top + subCards[5].height * 0.5 };
        stepPointer({ kCenter.x + simArc.arcRadius * std::cos(simGapAngle), kCenter.y + simArc.arcRadius * std::sin(simGapAngle) });
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == -1);
        stepPointer(card5);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && selectedSub == 5);

        // Now move directly onto Recycle Bin icon -> MUST immediately leave Downloads and select Recycle Bin!
        stepPointer(GetSlotCenter(kCenter.x, kCenter.y, kRingR, recycleSlot, slotCount));
        CHECK(selectedSlot == recycleSlot && expandedSlot == -1 && selectedSub == -1);

        // Re-hover Downloads, then move directly onto Explorer icon -> MUST immediately switch to Explorer!
        stepPointer(dlIcon);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot);
        stepPointer(GetSlotCenter(kCenter.x, kCenter.y, kRingR, explorerSlot, slotCount));
        CHECK(selectedSlot == explorerSlot && expandedSlot == -1 && selectedSub == -1);

        // Hover Collection (slot 4), expand it, then move directly onto Downloads -> switches cleanly!
        stepPointer(GetSlotCenter(kCenter.x, kCenter.y, kRingR, collectionSlot, slotCount));
        CHECK(selectedSlot == collectionSlot && expandedSlot == collectionSlot && subCards.size() == 5);
        stepPointer(dlIcon);
        CHECK(selectedSlot == downloadsSlot && expandedSlot == downloadsSlot && subCards.size() == 8);

        // Move back to center deadzone -> clears everything!
        stepPointer(kCenter);
        CHECK(selectedSlot == -1 && expandedSlot == -1 && selectedSub == -1);
    }
}

static void TestCrashGuard()
{
    CrashGuardState s{};
    s = AdvanceCrashGuard(s, true, 1000);
    CHECK(!s.disabled && s.abnormalExitCount == 1);
    s = AdvanceCrashGuard(s, true, 2000);
    CHECK(!s.disabled && s.abnormalExitCount == 2);
    s = AdvanceCrashGuard(s, false, 2001);
    CHECK(!s.disabled && s.abnormalExitCount == 2);
    s = AdvanceCrashGuard(s, true, 1000 + kCrashLoopWindow100ns + 5000);
    CHECK(!s.disabled && s.abnormalExitCount == 1);
    s = AdvanceCrashGuard(s, true, 1000 + kCrashLoopWindow100ns + 6000);
    s = AdvanceCrashGuard(s, true, 1000 + kCrashLoopWindow100ns + 7000);
    CHECK(s.disabled && s.abnormalExitCount == 3);
}

static void TestSearchScoring()
{
    CHECK(ScoreSearchMatch(L"Visual Studio Code", L"vis") == 0);
    CHECK(ScoreSearchMatch(L"Visual Studio Code", L"code") == 1);
    CHECK(ScoreSearchMatch(L"PowerShell", L"ps") == 2);
    CHECK(ScoreSearchMatch(L"Visual Studio Code", L"vs") == 2);
    CHECK(ScoreSearchMatch(L"File Explorer", L"fe") == 2);
    CHECK(ScoreSearchMatch(L"Notepad", L"tepa") == 3);
    CHECK(ScoreSearchMatch(L"Notepad", L"xyz") == -1);
    CHECK(ScoreSearchMatch(L"Helium", L"") == -1);

    const std::vector<std::wstring> heliumAliases = { L"chrome", L"Helium", L"Chromium" };
    CHECK(ScoreAliases(heliumAliases, L"hel") == 0);
    CHECK(ScoreAliases(heliumAliases, L"chr") == 0);
    CHECK(ScoreAliases(heliumAliases, L"ium") == 3);
    CHECK(ScoreAliases(heliumAliases, L"nomatch") == -1);
}

static void TestClockFormat()
{
    SYSTEMTIME st{};
    st.wYear = 2026;
    st.wMonth = 11;
    st.wDay = 20; // Friday
    st.wHour = 16;
    st.wMinute = 2;
    st.wSecond = 9;

    const std::wstring compact = FormatClock(st, L"ddd d MMM  h:mm tt", ClockCasing::CompactLower);
    CHECK(compact == L"Fri 20 Nov  4:02 pm");

    const std::wstring exact = FormatClock(st, L"ddd d MMM  h:mm tt", ClockCasing::Exact);
    CHECK(exact == L"Fri 20 Nov  4:02 PM");

    const std::wstring upper = FormatClock(st, L"dddd dd MMMM HH:mm:ss", ClockCasing::Upper);
    CHECK(upper == L"FRIDAY 20 NOVEMBER 16:02:09");
}

static void TestConfigRoundTrip()
{
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(_countof(tempDir), tempDir);
    const std::wstring testDir = std::wstring(tempDir) + L"WinRingsTest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(testDir.c_str(), nullptr);

    const std::wstring cfgPath = testDir + L"\\config.json";
    SwitcherConfig orig = SwitcherConfig::CreateDefault();
    CHECK(orig.version == 3);
    CHECK(orig.theme == RingTheme::A4S);
    CHECK(orig.glassLevel == 50);
    CHECK(orig.lightDarkMode == 0);
    CHECK(orig.accentColor == L"#0078D4");
    CHECK(orig.mediaIslandFullscreenMode == MediaIslandFullscreenMode::SyncWithTaskbar);
    CHECK(orig.mediaIslandOffsetXPct == -1);
    CHECK(orig.SaveToFile(cfgPath));
    SwitcherConfig loadedDefault;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, loadedDefault));
    CHECK(loadedDefault.version == 3);
    CHECK(loadedDefault.theme == RingTheme::A4S);
    CHECK(loadedDefault.glassLevel == 50);
    CHECK(loadedDefault.lightDarkMode == 0);

    orig.mouseTrigger = MouseTriggerButton::Middle;
    orig.topBarHeightDip = 32;
    orig.topBarClockFormat = L"ddd d MMM  h:mm tt";
    orig.topBarClockCasing = ClockCasing::CompactLower;
    orig.disableRingBlur = true;
    orig.selectionIndicator = SelectionIndicatorStyle::PointerLine;
    orig.winKeyTrigger = false;
    orig.directSwitchEnabled = true;
    orig.directSwitchModifiers = MOD_WIN;
    orig.directSwitchVk = 'F';
    orig.shakeTriggerEnabled = true;
    orig.shakeMinReversals = 5;
    orig.hotCornerEnabled = true;
    orig.hotCornerPos = HotCornerPos::TopRight;
    orig.hotCornerAction = HotCornerAction::OpenRing;
    orig.fullWorkAreaEnabled = false;
    orig.mediaIslandEnabled = true;
    orig.mediaIslandPos = MediaIslandPos::Left;
    orig.mediaIslandHoverExpand = false;
    orig.mediaIslandMarquee = false;
    orig.mediaIslandFullscreenMode = MediaIslandFullscreenMode::GamesOnly;
    orig.mediaIslandOffsetXPct = 42;
    orig.theme = RingTheme::DarkWood;
    orig.glassLevel = 86;
    orig.lightDarkMode = 15;
    orig.accentColor = L"#FF5522";
    orig.menuKeyEnabled = true;
    orig.interceptWinTab = true;
    orig.pinnedSlots.push_back(PinnedSlotConfig{
        L"col-1", SlotKind::Collection, L"Dev", L"", L"", 8,
        { PinnedSlotConfig{ L"site-1", SlotKind::Website, L"Example", L"https://example.com", L"icons\\example.ico", 8, {} } }
    });

    CHECK(orig.SaveToFile(cfgPath));
    SwitcherConfig loaded;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, loaded));
    CHECK(loaded.mouseTrigger == MouseTriggerButton::Middle);
    CHECK(loaded.topBarHeightDip == 32);
    CHECK(loaded.topBarClockFormat == L"ddd d MMM  h:mm tt");
    CHECK(loaded.topBarClockCasing == ClockCasing::CompactLower);
    CHECK(loaded.disableRingBlur == true);
    CHECK(loaded.selectionIndicator == SelectionIndicatorStyle::PointerLine);
    CHECK(loaded.winKeyTrigger == false);
    CHECK(loaded.directSwitchModifiers == MOD_WIN);
    CHECK(loaded.shakeMinReversals == 5);
    CHECK(loaded.hotCornerPos == HotCornerPos::TopRight);
    CHECK(loaded.hotCornerAction == HotCornerAction::OpenRing);
    CHECK(loaded.fullWorkAreaEnabled == false);
    CHECK(loaded.mediaIslandPos == MediaIslandPos::Left);
    CHECK(loaded.mediaIslandHoverExpand == false);
    CHECK(loaded.mediaIslandMarquee == false);
    CHECK(loaded.mediaIslandFullscreenMode == MediaIslandFullscreenMode::GamesOnly);
    CHECK(loaded.mediaIslandOffsetXPct == 42);
    CHECK(loaded.theme == RingTheme::DarkWood);
    CHECK(loaded.glassLevel == 86);
    CHECK(loaded.lightDarkMode == 15);
    CHECK(loaded.accentColor == L"#FF5522");
    CHECK(loaded.menuKeyEnabled == true);
    CHECK(loaded.interceptWinTab == true);
    CHECK(loaded.pinnedSlots.size() == 4);
    CHECK(loaded.pinnedSlots[3].kind == SlotKind::Collection);
    CHECK(loaded.pinnedSlots[3].items.size() == 1);
    CHECK(loaded.pinnedSlots[3].items[0].pathOrUrl == L"https://example.com");

    orig.theme = RingTheme::Blue;
    orig.glassLevel = 45;
    orig.lightDarkMode = 80;
    CHECK(orig.SaveToFile(cfgPath));
    SwitcherConfig loadedBlue;
    CHECK(SwitcherConfig::LoadFromFile(cfgPath, loadedBlue));
    CHECK(loadedBlue.theme == RingTheme::Blue);
    CHECK(loadedBlue.glassLevel == 45);
    CHECK(loadedBlue.lightDarkMode == 80);

    SwitcherConfig clamped;
    CHECK(SwitcherConfig::ParseJsonUtf8(
        "{\"version\":3,\"theme\":\"DarkWood\",\"glassLevel\":150,\"lightDarkMode\":-25}",
        clamped));
    CHECK(clamped.theme == RingTheme::DarkWood);
    CHECK(clamped.glassLevel == 100);
    CHECK(clamped.lightDarkMode == 0);

    for (const char* legacyJson : {
        "{\"version\":2,\"theme\":\"LiquidGlass\"}",
        "{\"version\":2,\"theme\":\"Glass\"}",
        "{\"version\":2,\"theme\":\"Aura\"}"
    })
    {
        SwitcherConfig legacyCfg;
        CHECK(SwitcherConfig::ParseJsonUtf8(legacyJson, legacyCfg));
        CHECK(legacyCfg.theme == RingTheme::A4S);
    }

    SwitcherConfig bad;
    CHECK(!SwitcherConfig::ParseJsonUtf8("{ not valid json", bad));

    DeleteFileW(cfgPath.c_str());
    RemoveDirectoryW(testDir.c_str());
}

static void TestThemeMaterialEngine()
{
    SwitcherConfig a4s = SwitcherConfig::CreateDefault();
    const RingMaterialSpec mA4s = ComputeRingMaterial(a4s, 1.0);
    CHECK(std::abs(mA4s.tintR - (32.0f / 255.0f)) < 0.002f);
    CHECK(std::abs(mA4s.tintAlpha - (128.0f / 255.0f)) < 0.002f);
    CHECK(std::abs(mA4s.blurSigma - 30.0f) < 0.05f);
    CHECK(std::abs(mA4s.luminosityR - (38.0f / 255.0f)) < 0.002f);
    CHECK(mA4s.luminosityAlpha >= 0.88f);
    CHECK(mA4s.noiseOpacity > 0.008f && mA4s.noiseOpacity <= 0.016f);
    CHECK(mA4s.borderColorRef == RGB(62, 62, 62));
    CHECK(mA4s.ringStrokeWidth <= 0.85f);
    CHECK(mA4s.isLightText);
    CHECK(std::abs(mA4s.hoverRevealColor.r - (52.0f / 255.0f)) < 0.005f);
    CHECK(std::abs(mA4s.hoverRevealColor.g - (99.0f / 255.0f)) < 0.005f);
    CHECK(std::abs(mA4s.hoverRevealColor.b - (141.0f / 255.0f)) < 0.005f);

    SwitcherConfig a4sLight = a4s;
    a4sLight.lightDarkMode = 100;
    const RingMaterialSpec mLight = ComputeRingMaterial(a4sLight, 1.0);
    CHECK(mLight.tintR > 0.90f && mLight.tintG > 0.90f && mLight.tintB > 0.90f);
    CHECK(!mLight.isLightText);
    CHECK(mLight.textColor.r < 0.20f);

    SwitcherConfig customAcc = a4s;
    customAcc.accentColor = L"#FF5522";
    const RingMaterialSpec mAcc = ComputeRingMaterial(customAcc, 1.0);
    CHECK(mAcc.accentColorRef == RGB(0xFF, 0x55, 0x22));
    CHECK(std::abs(mAcc.hoverRevealColor.r - 1.0f) < 0.005f);

    SwitcherConfig dw = a4s;
    dw.theme = RingTheme::DarkWood;
    dw.glassLevel = 86;
    const RingMaterialSpec mDw = ComputeRingMaterial(dw, 1.0);
    CHECK(mDw.tintAlpha > 0.80f);
    CHECK(mDw.blurSigma < 3.0f);

    SwitcherConfig blue = a4s;
    blue.theme = RingTheme::Blue;
    blue.glassLevel = 45;
    const RingMaterialSpec mBlue = ComputeRingMaterial(blue, 1.0);
    CHECK(mBlue.tintB > mBlue.tintR);
    CHECK(mBlue.accentColorRef == RGB(49, 134, 255));
    CHECK(mBlue.blurSigma > 25.0f);

    const std::vector<uint8_t> noise = GenerateAcrylicNoiseBgra256();
    CHECK(noise.size() == 256u * 256u * 4u);
    uint8_t minLuma = 255, maxLuma = 0;
    bool premulOk = true;
    for (size_t i = 0; i < noise.size(); i += 4u)
    {
        if (noise[i + 0] > noise[i + 3] || noise[i + 1] > noise[i + 3] || noise[i + 2] > noise[i + 3])
            premulOk = false;
        if (noise[i + 0] < minLuma) minLuma = noise[i + 0];
        if (noise[i + 0] > maxLuma) maxLuma = noise[i + 0];
    }
    CHECK(premulOk);
    CHECK(maxLuma > minLuma + 80);
}

static void TestSlotMatchingAndPreviews()
{
    CHECK(IsExplorerExecutablePath(L"C:\\Windows\\explorer.exe"));
    CHECK(IsExplorerExecutablePath(L"C:/Windows/Explorer.EXE"));
    CHECK(IsExplorerExecutablePath(L"explorer.exe"));
    CHECK(!IsExplorerExecutablePath(L"C:\\Windows\\notepad.exe"));
    CHECK(!IsExplorerExecutablePath(L""));

    // Folder matching against Explorer window titles (leaf name, suffix strip, full path, negative cases)
    const std::wstring dlPath = L"C:\\Users\\Administrator\\Downloads";
    CHECK(ExplorerWindowMatchesFolder(L"Downloads - File Explorer", dlPath));
    CHECK(ExplorerWindowMatchesFolder(L"downloads - file explorer", dlPath + L"\\"));
    CHECK(ExplorerWindowMatchesFolder(L"Downloads", dlPath));
    CHECK(ExplorerWindowMatchesFolder(L"C:\\Users\\Administrator\\Downloads - File Explorer", L"C:/Users/Administrator/Downloads/"));
    CHECK(!ExplorerWindowMatchesFolder(L"Documents - File Explorer", dlPath));
    CHECK(!ExplorerWindowMatchesFolder(L"This PC - File Explorer", dlPath));
    CHECK(!ExplorerWindowMatchesFolder(L"Recycle Bin - File Explorer", dlPath));
    CHECK(!ExplorerWindowMatchesFolder(L"C:", L"C:\\"));
    CHECK(ExplorerWindowMatchesFolder(L"C:\\ - File Explorer", L"C:\\"));

    // Recycle Bin matching
    CHECK(ExplorerWindowMatchesRecycleBin(L"Recycle Bin - File Explorer"));
    CHECK(ExplorerWindowMatchesRecycleBin(L"recycle bin"));
    CHECK(ExplorerWindowMatchesRecycleBin(L"Corbeille - File Explorer", L"Corbeille"));
    CHECK(!ExplorerWindowMatchesRecycleBin(L"Downloads - File Explorer"));
    CHECK(!ExplorerWindowMatchesRecycleBin(L""));

    // Window PiP previews: App only; Folder and RecycleBin must never open DWM window previews
    CHECK(ShouldShowSlotWindowPreview(SlotKind::App, 1, true));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::App, 1, false));
    CHECK(ShouldShowSlotWindowPreview(SlotKind::App, 2, false));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::Folder, 1, true));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::Folder, 3, false));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::RecycleBin, 1, true));
    CHECK(!ShouldShowSlotWindowPreview(SlotKind::RecycleBin, 3, false));
}

static void TestDragSpringAndReorder()
{
    // Spring-loaded double-heartbeat timing: 500ms dwell + 420ms two-beat pulse + trigger
    const auto s0 = EvaluateSpringHover(0.0);
    CHECK(std::abs(s0.scaleMultiplier - 1.0) < 1e-6 && !s0.pulsing && !s0.shouldActivate);
    const auto s250 = EvaluateSpringHover(250.0);
    CHECK(std::abs(s250.scaleMultiplier - 1.0) < 1e-6 && !s250.pulsing && !s250.shouldActivate);
    const auto s499 = EvaluateSpringHover(499.0);
    CHECK(std::abs(s499.scaleMultiplier - 1.0) < 1e-6 && !s499.pulsing && !s499.shouldActivate);

    const auto b1Peak = EvaluateSpringHover(552.5);
    CHECK(b1Peak.pulsing && !b1Peak.shouldActivate && b1Peak.scaleMultiplier > 1.18);
    const auto b1Dip = EvaluateSpringHover(657.5);
    CHECK(b1Dip.pulsing && !b1Dip.shouldActivate && b1Dip.scaleMultiplier < 0.92);
    const auto b2Peak = EvaluateSpringHover(762.5);
    CHECK(b2Peak.pulsing && !b2Peak.shouldActivate && b2Peak.scaleMultiplier > 1.18);
    const auto b2Dip = EvaluateSpringHover(867.5);
    CHECK(b2Dip.pulsing && !b2Dip.shouldActivate && b2Dip.scaleMultiplier < 0.92);

    const auto trigger = EvaluateSpringHover(920.0);
    CHECK(!trigger.pulsing && trigger.shouldActivate && std::abs(trigger.scaleMultiplier - 1.0) < 1e-6);

    // Reorder target computation (8 slots total, 5 pinned)
    CHECK(ComputeReorderTargetSlotIndex(0.0, -90.0, 5, 8) == 0);   // 12 o'clock -> slot 0
    CHECK(ComputeReorderTargetSlotIndex(90.0, 0.0, 5, 8) == 2);    // 3 o'clock -> slot 2
    CHECK(ComputeReorderTargetSlotIndex(0.0, 90.0, 5, 8) == 4);    // 6 o'clock -> slot 4
    CHECK(ComputeReorderTargetSlotIndex(-90.0, 0.0, 5, 8) == 4);   // 9 o'clock (slot 6) clamped to pinnedCount - 1 (4)
    CHECK(ComputeReorderTargetSlotIndex(90.0, 0.0, 1, 8) == 0);    // <=1 pinned -> 0

    // Reorder vector element forward, backward, and no-op
    std::vector<int> v1{ 10, 20, 30, 40, 50 };
    ReorderVectorElement(v1, 0, 3);
    CHECK((v1 == std::vector<int>{ 20, 30, 40, 10, 50 }));

    std::vector<int> v2{ 10, 20, 30, 40, 50 };
    ReorderVectorElement(v2, 4, 1);
    CHECK((v2 == std::vector<int>{ 10, 50, 20, 30, 40 }));

    std::vector<int> v3{ 10, 20, 30, 40, 50 };
    ReorderVectorElement(v3, 2, 2);
    CHECK((v3 == std::vector<int>{ 10, 20, 30, 40, 50 }));
}

int wmain()
{
    TestRadialMath();
    TestDownloadsAndSubmenuHoverTransitions();
    TestSearchScoring();
    TestClockFormat();
    TestCrashGuard();
    TestConfigRoundTrip();
    TestThemeMaterialEngine();
    TestSlotMatchingAndPreviews();
    TestDragSpringAndReorder();

    if (g_failures == 0)
    {
        std::printf("WinRingsTests: ALL PASSED\n");
        return 0;
    }
    std::printf("WinRingsTests: %d FAILURE(S)\n", g_failures);
    return 1;
}
