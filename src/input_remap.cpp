// naruto - controller button remapping (see input_remap.h)

#include "input_remap.h"

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

REXCVAR_DECLARE(std::string, input_backend);

REXCVAR_DEFINE_STRING(pad_map_xbox, "", "Input/Remap",
                      "Xbox controllers' remapped buttons, as From=To pairs: \"A=B,B=A\"");
REXCVAR_DEFINE_STRING(pad_map_playstation, "", "Input/Remap",
                      "PlayStation controllers' remapped buttons, as From=To pairs");
REXCVAR_DEFINE_STRING(pad_map_nintendo, "", "Input/Remap",
                      "Nintendo controllers' remapped buttons, as From=To pairs");
REXCVAR_DEFINE_STRING(pad_map_other, "", "Input/Remap",
                      "Other controllers' (Steam Input's too) remapped buttons, as From=To pairs");

namespace input_remap {

namespace {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using namespace rex;         // X_STATUS, X_RESULT
using namespace rex::input;  // X_INPUT_* types and button bits

enum PadKind { kXbox, kPlayStation, kNintendo, kOther, kPadKinds };
constexpr const char* kKindNames[] = {"Xbox", "PlayStation", "Nintendo", "other"};

// The buttons a map names, in this order; the last two are the triggers.
constexpr std::string_view kButtonNames[] = {"Up", "Down", "Left", "Right", "Start", "Back",
                                             "LS", "RS",   "LB",   "RB",    "A",     "B",
                                             "X",  "Y",    "LT",   "RT"};
constexpr uint16_t kButtonBits[] = {
    X_INPUT_GAMEPAD_DPAD_UP,       X_INPUT_GAMEPAD_DPAD_DOWN,      X_INPUT_GAMEPAD_DPAD_LEFT,
    X_INPUT_GAMEPAD_DPAD_RIGHT,    X_INPUT_GAMEPAD_START,          X_INPUT_GAMEPAD_BACK,
    X_INPUT_GAMEPAD_LEFT_THUMB,    X_INPUT_GAMEPAD_RIGHT_THUMB,    X_INPUT_GAMEPAD_LEFT_SHOULDER,
    X_INPUT_GAMEPAD_RIGHT_SHOULDER, X_INPUT_GAMEPAD_A,             X_INPUT_GAMEPAD_B,
    X_INPUT_GAMEPAD_X,             X_INPUT_GAMEPAD_Y};
constexpr int kButtons = 16, kLeftTrigger = 14, kRightTrigger = 15, kNone = -1;
// XInput's XINPUT_GAMEPAD_TRIGGER_THRESHOLD: a trigger counts as a press past it.
constexpr uint8_t kTriggerThreshold = 30;

int ButtonIndex(std::string_view name) {
  for (int i = 0; i < kButtons; ++i) {
    if (kButtonNames[i] == name) {
      return i;
    }
  }
  return name == "None" ? kNone : kButtons;
}

// What each button sends, kNone for nothing.
struct ButtonMap {
  std::array<int, kButtons> target;
  bool identity = true;
};

ButtonMap ParseMap(std::string_view text, PadKind kind) {
  ButtonMap map;
  for (int i = 0; i < kButtons; ++i) {
    map.target[i] = i;
  }
  while (!text.empty()) {
    size_t comma = text.find(',');
    std::string_view pair = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    size_t equals = pair.find('=');
    int from = equals == std::string_view::npos ? kButtons : ButtonIndex(pair.substr(0, equals));
    int to = equals == std::string_view::npos ? kButtons : ButtonIndex(pair.substr(equals + 1));
    if (from < 0 || from >= kButtons || to >= kButtons) {
      REXLOG_WARN("[input] ignoring \"{}\" in the {} controller map", pair, kKindNames[kind]);
      continue;
    }
    map.target[from] = to;
    map.identity = map.identity && to == from;
  }
  return map;
}

void Apply(const ButtonMap& map, X_INPUT_GAMEPAD& pad) {
  uint16_t buttons = pad.buttons;
  uint8_t triggers[2] = {pad.left_trigger, pad.right_trigger};
  uint16_t out_buttons = buttons & X_INPUT_GAMEPAD_GUIDE;
  uint8_t out_triggers[2] = {0, 0};
  for (int from = 0; from < kButtons; ++from) {
    int to = map.target[from];
    bool trigger = from >= kLeftTrigger;
    // A trigger sent to a trigger keeps its travel; a button press sends it
    // all the way.
    uint8_t value = trigger ? triggers[from - kLeftTrigger]
                            : (buttons & kButtonBits[from]) ? uint8_t(255) : uint8_t(0);
    if (to == kNone || !value) {
      continue;
    }
    if (to >= kLeftTrigger) {
      uint8_t& out = out_triggers[to - kLeftTrigger];
      out = std::max(out, value);
    } else if (value > kTriggerThreshold) {
      out_buttons |= kButtonBits[to];
    }
  }
  pad.buttons = out_buttons;
  pad.left_trigger = out_triggers[0];
  pad.right_trigger = out_triggers[1];
}

PadKind KindOf(const DeviceInfo& info) {
  Uint16 vendor = 0;
  SDL_GetJoystickGUIDInfo(SDL_StringToGUID(info.guid.c_str()), &vendor, nullptr, nullptr,
                          nullptr);
  switch (vendor) {
    case 0x045E:  // Microsoft
      return kXbox;
    case 0x054C:  // Sony
      return kPlayStation;
    case 0x057E:  // Nintendo
      return kNintendo;
    default:
      return kOther;
  }
}

// The SDL driver, with its controllers' buttons remapped.
class RemapDriver final : public rex::input::InputDriver {
 public:
  explicit RemapDriver(std::unique_ptr<rex::input::InputDriver> inner)
      : InputDriver(nullptr, 0), inner_(std::move(inner)) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }  // the SDL driver's is done

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    size_t first = out.size();
    inner_->EnumerateDevices(out);
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = first; i < out.size(); ++i) {
      if (kinds_.emplace(out[i].id, KindOf(out[i])).second) {
        REXLOG_INFO("[input] {}: {} controller map", out[i].name, kKindNames[kinds_[out[i].id]]);
      }
    }
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    X_RESULT result = inner_->GetDeviceState(id, out_state);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }
    if (!is_active()) {
      // As the SDL driver does without focus: an untouched controller.
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
      return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto kind = kinds_.find(id);
    const ButtonMap& map = MapFor(kind == kinds_.end() ? kOther : kind->second);
    if (!map.identity) {
      Apply(map, out_state->gamepad);
    }
    return result;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    return inner_->GetDeviceCapabilities(id, flags, out_caps);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    return inner_->SetDeviceVibration(id, vibration);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override {
    return inner_->GetDeviceKeystroke(id, flags, out_keystroke);
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

 private:
  // The kind's map, parsed again when its setting changes; mutex_ is held.
  const ButtonMap& MapFor(PadKind kind) {
    const std::string* settings[kPadKinds] = {
        &REXCVAR_GET(pad_map_xbox), &REXCVAR_GET(pad_map_playstation),
        &REXCVAR_GET(pad_map_nintendo), &REXCVAR_GET(pad_map_other)};
    if (!parsed_[kind] || *settings[kind] != texts_[kind]) {
      texts_[kind] = *settings[kind];
      maps_[kind] = ParseMap(texts_[kind], kind);
      parsed_[kind] = true;
    }
    return maps_[kind];
  }

  std::unique_ptr<rex::input::InputDriver> inner_;
  std::mutex mutex_;
  std::map<DeviceId, PadKind> kinds_;
  std::array<bool, kPadKinds> parsed_{};
  std::array<std::string, kPadKinds> texts_;
  std::array<ButtonMap, kPadKinds> maps_;
};

// NARUTO_INPUT_SCRIPT=1, for driving menus from scripts: the stand-in
// controller (player 1 when none is connected) presses the buttons named in
// out/input-request (or $NARUTO_OUT_DIR/input-request), separated by spaces
// or lines, in order, each held 150 ms then released 150 ms; the file is
// deleted once read. Names as for the maps above: A, Start, Up and so on.
class ScriptDriver final : public rex::input::InputDriver {
 public:
  ScriptDriver()
      : InputDriver(nullptr, 1),
        inner_(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1)) {
    const char* out = std::getenv("NARUTO_OUT_DIR");
    path_ = std::string(out && *out ? out : "out") + "/input-request";
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    X_RESULT result = inner_->GetDeviceState(id, out_state);
    if (result != X_ERROR_SUCCESS || !out_state) {
      return result;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    if (queue_.empty() && now - polled_ > std::chrono::milliseconds(100)) {
      polled_ = now;
      std::ifstream file(path_);
      if (file) {
        std::stringstream text;
        text << file.rdbuf();
        file.close();
        std::remove(path_.c_str());
        std::string name;
        while (text >> name) {
          int index = ButtonIndex(name);
          if (index >= 0 && index < kButtons) {
            queue_.push_back(index);
          } else {
            REXLOG_WARN("[input] input-request: unknown button {}", name);
          }
        }
        started_ = now;
      }
    }
    bool pressed = false;
    if (!queue_.empty()) {
      auto held = now - started_;
      if (held < std::chrono::milliseconds(150)) {
        pressed = true;
        int index = queue_.front();
        if (index == kLeftTrigger) {
          out_state->gamepad.left_trigger = 0xFF;
        } else if (index == kRightTrigger) {
          out_state->gamepad.right_trigger = 0xFF;
        } else {
          out_state->gamepad.buttons = uint16_t(out_state->gamepad.buttons | kButtonBits[index]);
        }
      } else if (held >= std::chrono::milliseconds(300)) {
        queue_.pop_front();
        started_ = now;
      }
    }
    // XInput's packet number changes whenever the state does.
    if (pressed != pressed_) {
      pressed_ = pressed;
      ++packet_;
    }
    out_state->packet_number = packet_;
    return result;
  }

  X_STATUS Setup() override { return inner_->Setup(); }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override { inner_->EnumerateDevices(out); }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    return inner_->GetDeviceCapabilities(id, flags, out_caps);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    return inner_->SetDeviceVibration(id, vibration);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override {
    return inner_->GetDeviceKeystroke(id, flags, out_keystroke);
  }

 private:
  std::unique_ptr<rex::input::InputDriver> inner_;
  std::string path_;
  std::mutex mutex_;
  std::deque<int> queue_;
  std::chrono::steady_clock::time_point polled_{}, started_{};
  bool pressed_ = false;
  uint32_t packet_ = 0;
};

}  // namespace

// As rex::input::CreateDefaultInputSystem, with the SDL driver wrapped.
std::unique_ptr<rex::input::InputSystem> CreateInputSystem(bool tool_mode) {
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);
  if (!tool_mode) {
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
      if (sdl->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::make_unique<RemapDriver>(std::move(sdl)));
      }
    }
    auto mnk = std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
    if (mnk->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk));
    }
  }
  const char* script = std::getenv("NARUTO_INPUT_SCRIPT");
  if (!tool_mode && script && script[0] == '1') {
    input->AddDriver(std::make_unique<ScriptDriver>());
  } else {
    input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, tool_mode ? 0 : 1));
  }
  input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  return input;
}

}  // namespace input_remap
