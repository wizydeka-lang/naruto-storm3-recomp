// naruto - controller button remapping
//
// The game's input system: the SDK's default drivers, with the buttons of
// SDL controllers remapped by the kind of controller (its USB vendor). Each
// kind has a setting, pad_map_xbox, pad_map_playstation, pad_map_nintendo
// and pad_map_other, listing the buttons that send something else, as
// "From=To" pairs: "A=B,B=A,LB=None". Buttons are named by their Xbox
// position (SDL reports every controller that way: a PlayStation controller's
// cross is A, a Nintendo controller's B is A too): A B X Y LB RB LT RT Back
// Start LS RS Up Down Left Right, and None sends nothing. The launcher
// (tools/launcher) writes them; keyboard controls are the SDK's keybind_*
// settings.

#pragma once

#include <memory>

#include <rex/input/input_system.h>

namespace input_remap {

std::unique_ptr<rex::input::InputSystem> CreateInputSystem(bool tool_mode);

}  // namespace input_remap
