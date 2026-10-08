// VITA5 app — the dock theme: one Theme struct that restyles the kit.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Identity: OLED-dark "dock at night" — deep indigo glass panels, a cyan
// signal accent and a warm amber for waiting states. The backdrop is the
// kit's animated aurora, so the page is never static.

#pragma once
#include "ui/theme.hpp"

namespace vd5
{

// Shared palette values (screens draw custom art with the same colours).
namespace color
{
constexpr std::uint32_t kPage = 0x070b16;
constexpr std::uint32_t kPanel = 0x111c33;
constexpr std::uint32_t kPanelHigh = 0x1a2a47;
constexpr std::uint32_t kInk = 0xeaf2ff;
constexpr std::uint32_t kMuted = 0x8fa6c8;
constexpr std::uint32_t kSignal = 0x2fe0c9; // connected / live
constexpr std::uint32_t kWaiting = 0xffb347; // waiting for the Vita
constexpr std::uint32_t kAlert = 0xff5d6c;  // error / disconnected
constexpr std::uint32_t kSuccess = 0x3ddc84; // done / saved
constexpr std::uint32_t kDeep = 0x04070f;
} // namespace color

const hui::ui::Theme &dock_theme();

} // namespace vd5
