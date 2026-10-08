// VITA5 app — in-app input test harness screen (scripted pad + touch tests).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A live, in-app twin of the payload test senders (payload/padtest.c,
// payload/touchtest.c, payload/touchswipe.c): scripted button tours and
// touch patterns run on a background thread through the core USB transport
// while the screen shows a per-step status log. See screen_inputtest.cpp for
// the scripts and the transport contract.
//
// REGISTRATION (the parent app owns screens.hpp and the screen switch):
//   1. Declare the factory in app/src/screens.hpp next to the other
//      factories:
//          std::unique_ptr<Screen> make_inputtest_screen(AppServices &services);
//      (or include this header there).
//   2. Construct the screen where the app builds its screens and give it a
//      way in (e.g. a "Input test" entry on the settings screen).
//   3. Exit: Back currently leaves via AppServices::go_live (the single line
//      marked TODO(exit) in screen_inputtest.cpp). Wire a dedicated request
//      (e.g. close_inputtest) and swap that line if you prefer.

#pragma once
#include "screens.hpp"

namespace vd5
{

std::unique_ptr<Screen> make_inputtest_screen(AppServices &services);

} // namespace vd5
