#!/usr/bin/env python3
"""Launcher for the Naruto Storm 3 recompilation.

Sets up how the game looks (resolution, upscaling, anti-aliasing) and is
controlled (controller buttons per kind of controller, keyboard keys), then
starts it with the native renderer through tools/run-radeon.sh. Settings are
kept in ~/.config/naruto-storm3/launcher.json.

  python3 tools/launcher/naruto_launcher.py            the launcher
  python3 tools/launcher/naruto_launcher.py --play     start the game right away
  python3 tools/launcher/naruto_launcher.py --install  add the app menu entries

--install takes the artwork from your copy of the game (assets/nxeart) for the
menu icons and the launcher's background; nothing of it is in the repository.
Needs PySide6 (part of Bazzite and Fedora); controllers are read through SDL 3.
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_SCRIPT = ROOT / "tools" / "run-radeon.sh"
CONFIG_FILE = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") \
    / "naruto-storm3" / "launcher.json"
DATA_HOME = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
ART_DIR = DATA_HOME / "naruto-storm3"
LOG_FILE = ART_DIR / "last-run.log"
GAME_ID = "naruto-storm3"
LAUNCHER_ID = "naruto-storm3-launcher"

TITLE = "Naruto Shippuden: Ultimate Ninja Storm 3"

# (label, NARUTO_NATIVE_VIEW_SCALE): the game renders 720p.
RESOLUTIONS = [("720p", 1.0), ("1080p", 1.5), ("1440p", 2.0)]
# (NARUTO_NATIVE_VIEW_ASPECT, label): the shape the picture is shown at.
ASPECTS = [("16:9", "16:9"), ("16:10", "16:10"), ("21:9", "21:9"), ("32:9", "32:9"),
           ("4:3", "4:3"), ("fill", "Fill screen")]
UPSCALERS = [("fsr", "FSR 1"), ("cas", "CAS"), ("linear", "Bilinear")]
UPSCALER_HINTS = {
    "fsr": "AMD FidelityFX Super Resolution 1: sharp edge-aware upscaling to your screen.",
    "cas": "AMD Contrast Adaptive Sharpening: scales lightly and brings back fine detail.",
    "linear": "Plain smooth scaling, no sharpening. The cheapest.",
}
GPUS = [("nvidia", "NVIDIA", "/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json"),
        ("radeon", "AMD Radeon", "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json")]

# Buttons as the game names them (src/input_remap.h), by Xbox position.
BUTTONS = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "LS", "RS",
           "Up", "Down", "Left", "Right"]
KINDS = [("xbox", "Xbox"), ("playstation", "PlayStation"), ("nintendo", "Nintendo"),
         ("other", "Steam / Other")]
_DPAD = {"Up": "D-pad ↑", "Down": "D-pad ↓", "Left": "D-pad ←", "Right": "D-pad →"}
PAD_LABELS = {
    "xbox": {"A": "A", "B": "B", "X": "X", "Y": "Y", "LB": "LB", "RB": "RB", "LT": "LT",
             "RT": "RT", "Back": "View", "Start": "Menu", "LS": "Left stick click",
             "RS": "Right stick click", **_DPAD},
    "playstation": {"A": "✕  Cross", "B": "○  Circle", "X": "□  Square", "Y": "△  Triangle",
                    "LB": "L1", "RB": "R1", "LT": "L2", "RT": "R2", "Back": "Create / Share",
                    "Start": "Options", "LS": "L3", "RS": "R3", **_DPAD},
    "nintendo": {"A": "B  (bottom)", "B": "A  (right)", "X": "Y  (left)", "Y": "X  (top)",
                 "LB": "L", "RB": "R", "LT": "ZL", "RT": "ZR", "Back": "−  Minus",
                 "Start": "+  Plus", "LS": "Left stick click", "RS": "Right stick click",
                 **_DPAD},
    "other": {"A": "A  (bottom)", "B": "B  (right)", "X": "X  (left)", "Y": "Y  (top)",
              "LB": "LB / L1", "RB": "RB / R1", "LT": "LT / L2", "RT": "RT / R2",
              "Back": "Back / View", "Start": "Start / Menu", "LS": "Left stick click",
              "RS": "Right stick click", **_DPAD},
}
# What the game is sent, as the Xbox 360 controller it expects.
OUTPUT_LABELS = {"A": "A", "B": "B", "X": "X", "Y": "Y", "LB": "LB", "RB": "RB", "LT": "LT",
                 "RT": "RT", "Back": "Back", "Start": "Start", "LS": "Left stick click",
                 "RS": "Right stick click", **_DPAD, "None": "Nothing"}

# Keyboard: (setting after keybind_, label, default keys), grouped. The keys
# are the SDK's names (rex/ui/keybinds); WASD moves, IJKL are the face
# buttons in the Xbox diamond.
KEY_GROUPS = [
    ("Face buttons", [("a", "A", ["K"]), ("b", "B", ["L"]), ("x", "X", ["J"]),
                      ("y", "Y", ["I"])]),
    ("Shoulders and triggers", [("left_shoulder", "LB", ["U"]),
                                ("right_shoulder", "RB", ["O"]),
                                ("left_trigger", "LT", ["Q"]),
                                ("right_trigger", "RT", ["E"])]),
    ("Menu", [("start", "Start", ["Return"]), ("back", "Back", ["Tab"])]),
    ("Left stick", [("lstick_up", "Up", ["W"]), ("lstick_down", "Down", ["S"]),
                    ("lstick_left", "Left", ["A"]), ("lstick_right", "Right", ["D"]),
                    ("lstick_press", "Click", ["F"])]),
    ("Right stick (camera)", [("rstick_up", "Up", ["Numpad8"]),
                              ("rstick_down", "Down", ["Numpad2"]),
                              ("rstick_left", "Left", ["Numpad4"]),
                              ("rstick_right", "Right", ["Numpad6"]),
                              ("rstick_press", "Click", ["Numpad5"])]),
    ("D-pad", [("dpad_up", "Up", ["Up"]), ("dpad_down", "Down", ["Down"]),
               ("dpad_left", "Left", ["Left"]), ("dpad_right", "Right", ["Right"])]),
]
KEY_ACTIONS = [action for _, actions in KEY_GROUPS for action in actions]
KEY_LABELS = {name: f"{group}: {label}" if len(label) < 6 else label
              for group, actions in KEY_GROUPS for name, label, _ in actions}
KEY_SLOTS = 2  # keys per action
RESERVED_KEYS = {"F11": "switches fullscreen", "F12": "saves a renderer snapshot"}


def default_settings():
    gpu = next((key for key, _, icd in GPUS if Path(icd).exists()), "nvidia")
    return {
        "resolution": "1080p",
        "fullscreen": True,
        "aspect": "16:9",
        "upscaler": "fsr",
        "fxaa": False,
        "sharpness": 0.9,
        "gpu": gpu,
        "keyboard": True,
        "keys": {name: list(keys) for name, _, keys in KEY_ACTIONS},
        "pad_maps": {kind: {} for kind, _ in KINDS},
        "pad_kind": "xbox",
    }


def load_settings():
    settings = default_settings()
    try:
        saved = json.loads(CONFIG_FILE.read_text())
    except (OSError, ValueError):
        return settings
    for key, value in saved.items():
        if key in settings and isinstance(value, type(settings[key])):
            if isinstance(value, dict):
                settings[key].update(value)
            else:
                settings[key] = value
    if settings["aspect"] not in dict(ASPECTS):
        settings["aspect"] = "16:9"
    return settings


def save_settings(settings):
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(settings, indent=2))


def game_command(settings):
    """The command and environment that start the game with `settings`."""
    scale = dict(RESOLUTIONS).get(settings["resolution"], 1.5)
    env = dict(os.environ,
               NARUTO_NATIVE_VIEW_SCALE=f"{scale:g}",
               NARUTO_NATIVE_VIEW_FULLSCREEN="1" if settings["fullscreen"] else "0",
               NARUTO_NATIVE_VIEW_ASPECT=settings["aspect"],
               NARUTO_NATIVE_VIEW_UPSCALER=settings["upscaler"],
               NARUTO_NATIVE_VIEW_SHARPNESS=f"{settings['sharpness']:.2f}",
               NARUTO_NATIVE_VIEW_FXAA="1" if settings["fxaa"] else "0",
               # Matches the menu entry, for the window's icon.
               SDL_APP_ID=GAME_ID)
    command = ["bash", str(RUN_SCRIPT), "--native-only", f"--{settings['gpu']}", "--"]
    command.append(f"--mnk_mode={'true' if settings['keyboard'] else 'false'}")
    for name, _, _ in KEY_ACTIONS:
        keys = [key for key in settings["keys"].get(name, []) if key]
        command.append(f"--keybind_{name}={','.join(keys) or 'None'}")
    for kind, _ in KINDS:
        pairs = [f"{source}={target}" for source, target in
                 settings["pad_maps"].get(kind, {}).items() if source != target]
        # "A=A" changes nothing, and stands in for an empty value.
        command.append(f"--pad_map_{kind}={','.join(pairs) or 'A=A'}")
    return command, env


def check_game(settings):
    """None when the game is ready to start, else what is missing."""
    result = subprocess.run(["bash", str(RUN_SCRIPT), "--check", f"--{settings['gpu']}"],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return None
    return (result.stderr or result.stdout).strip().splitlines()[0]


def play_now():
    settings = load_settings()
    command, env = game_command(settings)
    os.chdir(ROOT)
    os.execvpe(command[0], command, env)


# ---------------------------------------------------------------------------
# Everything below needs Qt.

from PySide6 import QtCore, QtGui, QtWidgets  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sdl_pad  # noqa: E402

Qt = QtCore.Qt
ORANGE = QtGui.QColor("#ff8a1f")
ORANGE_DEEP = QtGui.QColor("#e2470f")
CHAKRA = QtGui.QColor("#3fb6ff")
INK = QtGui.QColor("#120f16")


def display_font(size, weight=QtGui.QFont.Black):
    font = QtGui.QFont("Noto Sans")
    font.setStretch(QtGui.QFont.Condensed)
    font.setWeight(weight)
    font.setPixelSize(size)
    return font


def spiral_path(center, turns, radius):
    """An Archimedean spiral, the swirl the launcher's art is built around."""
    path = QtGui.QPainterPath()
    steps = int(turns * 72)
    for i in range(steps + 1):
        t = i / steps
        angle = t * turns * 2 * math.pi
        point = center + QtCore.QPointF(math.cos(angle), math.sin(angle)) * (radius * t)
        if i == 0:
            path.moveTo(point)
        else:
            path.lineTo(point)
    return path


# ---------------------------------------------------------------------------
# Artwork: from the game's NXE art package, or drawn.

def extract_art():
    """Saves the game's dashboard artwork from assets/nxeart to ART_DIR: the
    background (1920x1080), the box art and the title icon. Returns which."""
    try:
        data = (ROOT / "assets" / "nxeart").read_bytes()
    except OSError:
        return []
    images = []
    for start_marker, end_marker, tail in ((b"\x89PNG", b"IEND", 8), (b"\xff\xd8\xff", b"\xff\xd9", 2)):
        start = data.find(start_marker)
        while start >= 0:
            end = data.find(end_marker, start)
            if end < 0:
                break
            image = QtGui.QImage.fromData(data[start:end + tail])
            if not image.isNull():
                images.append(image)
            start = data.find(start_marker, end)
    ART_DIR.mkdir(parents=True, exist_ok=True)
    saved = []
    wide = [image for image in images if image.width() >= 1280]
    if wide:
        wide[0].save(str(ART_DIR / "background.jpg"), "JPG", 92)
        saved.append("background")
    boxes = [image for image in images if 256 <= image.width() < 1280]
    if boxes:
        boxes[0].save(str(ART_DIR / "boxart.png"))
        saved.append("box art")
    icons = [image for image in images if image.width() < 256]
    if icons:
        icons[0].save(str(ART_DIR / "title.png"))
        saved.append("title icon")
    return saved


def load_art(name):
    image = QtGui.QImage(str(ART_DIR / name))
    return None if image.isNull() else image


def draw_swirl_emblem(painter, rect):
    """The drawn stand-in for the game's art: an orange chakra swirl."""
    painter.save()
    gradient = QtGui.QRadialGradient(rect.center(), rect.width() / 2)
    gradient.setColorAt(0, QtGui.QColor("#ffb347"))
    gradient.setColorAt(1, ORANGE_DEEP)
    painter.fillRect(rect, gradient)
    pen = QtGui.QPen(INK, rect.width() * 0.06, Qt.SolidLine, Qt.RoundCap)
    painter.setPen(pen)
    painter.drawPath(spiral_path(QtCore.QPointF(rect.center()), 2.6, rect.width() * 0.4))
    painter.restore()


def make_game_icon(size):
    """The game's menu icon: its box art in a rounded tile."""
    icon = QtGui.QImage(size, size, QtGui.QImage.Format_ARGB32_Premultiplied)
    icon.fill(Qt.transparent)
    painter = QtGui.QPainter(icon)
    painter.setRenderHints(QtGui.QPainter.Antialiasing | QtGui.QPainter.SmoothPixmapTransform)
    tile = QtCore.QRectF(size * 0.03, size * 0.03, size * 0.94, size * 0.94)
    clip = QtGui.QPainterPath()
    clip.addRoundedRect(tile, size * 0.18, size * 0.18)
    painter.setClipPath(clip)
    box = load_art("boxart.png")
    if box:
        # The square with Naruto in it, on the box art's left.
        side = min(box.width(), box.height()) * 0.94
        source = QtCore.QRectF(box.width() * 0.02, 0, side, side)
        painter.drawImage(tile, box, source)
    else:
        draw_swirl_emblem(painter, tile)
    shade = QtGui.QLinearGradient(0, tile.bottom() - size * 0.36, 0, tile.bottom())
    shade.setColorAt(0, QtGui.QColor(0, 0, 0, 0))
    shade.setColorAt(1, QtGui.QColor(10, 6, 4, 230))
    painter.fillRect(tile, shade)
    painter.setClipping(False)
    font = display_font(int(size * 0.17))
    font.setLetterSpacing(QtGui.QFont.PercentageSpacing, 104)
    text = QtGui.QPainterPath()
    metrics = QtGui.QFontMetricsF(font)
    label = "STORM 3"
    text.addText(QtCore.QPointF(size / 2 - metrics.horizontalAdvance(label) / 2,
                                tile.bottom() - size * 0.075), font, label)
    painter.setPen(QtGui.QPen(INK, size * 0.035, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
    painter.drawPath(text)
    painter.fillPath(text, QtGui.QColor("#fff4e6"))
    painter.setPen(QtGui.QPen(ORANGE, size * 0.025))
    painter.drawPath(clip)
    painter.end()
    return icon


def make_launcher_icon(size):
    """The launcher's icon: Naruto in a ring with a gear's teeth and a swirl,
    for the place you set the game up."""
    icon = QtGui.QImage(size, size, QtGui.QImage.Format_ARGB32_Premultiplied)
    icon.fill(Qt.transparent)
    painter = QtGui.QPainter(icon)
    painter.setRenderHints(QtGui.QPainter.Antialiasing | QtGui.QPainter.SmoothPixmapTransform)
    center = QtCore.QPointF(size / 2, size / 2)
    outer = size * 0.47
    # Gear teeth.
    painter.setPen(Qt.NoPen)
    painter.setBrush(ORANGE_DEEP)
    for i in range(10):
        painter.save()
        painter.translate(center)
        painter.rotate(i * 36)
        painter.drawRoundedRect(QtCore.QRectF(-size * 0.07, -outer, size * 0.14, size * 0.2),
                                size * 0.03, size * 0.03)
        painter.restore()
    ring = QtGui.QRadialGradient(center, outer * 0.92)
    ring.setColorAt(0.75, QtGui.QColor("#ffb347"))
    ring.setColorAt(1.0, ORANGE)
    painter.setBrush(ring)
    painter.drawEllipse(center, outer * 0.9, outer * 0.9)
    # The portrait.
    inner = outer * 0.72
    face = QtGui.QPainterPath()
    face.addEllipse(center, inner, inner)
    painter.save()
    painter.setClipPath(face)
    background = load_art("background.jpg")
    if background:
        # Naruto's face in the 1920x1080 key art.
        scale = background.width() / 1920
        source = QtCore.QRectF(845 * scale, 205 * scale, 310 * scale, 310 * scale)
        painter.drawImage(QtCore.QRectF(center.x() - inner, center.y() - inner, 2 * inner,
                                        2 * inner), background, source)
    else:
        draw_swirl_emblem(painter, QtCore.QRectF(center.x() - inner, center.y() - inner,
                                                 2 * inner, 2 * inner))
    painter.restore()
    painter.setBrush(Qt.NoBrush)
    painter.setPen(QtGui.QPen(INK, size * 0.03))
    painter.drawEllipse(center, inner, inner)
    # A swirl through the ring.
    painter.setPen(QtGui.QPen(INK, size * 0.022, Qt.SolidLine, Qt.RoundCap))
    painter.save()
    painter.translate(center)
    for i in range(3):
        painter.rotate(120)
        arc = QtCore.QRectF(-outer * 0.81, -outer * 0.81, outer * 1.62, outer * 1.62)
        painter.drawArc(arc, 0, 70 * 16)
    painter.restore()
    painter.end()
    return icon


def install_menu_entries(add_to_steam=False):
    """Adds the game and the launcher to the app menu, with icons made from
    the game's artwork. Returns a list of what was done."""
    done = []
    art = extract_art()
    done.append("Artwork from your game: " + ", ".join(art) if art
                else "No artwork in assets/nxeart; using drawn icons")
    icons = DATA_HOME / "icons" / "hicolor"
    for size in (64, 128, 256, 512):
        folder = icons / f"{size}x{size}" / "apps"
        folder.mkdir(parents=True, exist_ok=True)
        make_game_icon(size).save(str(folder / f"{GAME_ID}.png"))
        make_launcher_icon(size).save(str(folder / f"{LAUNCHER_ID}.png"))
    done.append(f"Icons in {icons}")
    applications = DATA_HOME / "applications"
    applications.mkdir(parents=True, exist_ok=True)
    launcher = f'"{sys.executable}" "{Path(__file__).resolve()}"'
    game_entry = applications / f"{GAME_ID}.desktop"
    game_entry.write_text(f"""[Desktop Entry]
Type=Application
Name={TITLE}
GenericName=Fighting game
Comment=Play with the settings from the Storm 3 launcher
Exec={launcher} --play
Path={ROOT}
Icon={GAME_ID}
Terminal=false
Categories=Game;ActionGame;
Keywords=naruto;storm;ninja;
StartupWMClass={GAME_ID}
Actions=Settings;

[Desktop Action Settings]
Name=Launcher and settings
Exec={launcher}
Icon={LAUNCHER_ID}
""")
    (applications / f"{LAUNCHER_ID}.desktop").write_text(f"""[Desktop Entry]
Type=Application
Name=Storm 3 Launcher
GenericName=Game launcher
Comment=Graphics, controller and keyboard settings for {TITLE}
Exec={launcher}
Path={ROOT}
Icon={LAUNCHER_ID}
Terminal=false
Categories=Game;
Keywords=naruto;storm;settings;controller;
StartupWMClass={LAUNCHER_ID}
""")
    done.append(f"Menu entries in {applications}")
    for tool in (["update-desktop-database", str(applications)], ["kbuildsycoca6"]):
        if shutil.which(tool[0]):
            subprocess.run(tool, capture_output=True)
    if add_to_steam:
        done.append(add_game_to_steam())
    return done


def add_game_to_steam():
    entry = DATA_HOME / "applications" / f"{GAME_ID}.desktop"
    if not shutil.which("steamos-add-to-steam"):
        return "steamos-add-to-steam isn't installed (it comes with Bazzite)"
    if not entry.exists():
        return "Add the menu entries first"
    result = subprocess.run(["steamos-add-to-steam", str(entry)], capture_output=True,
                            text=True)
    if result.returncode != 0:
        return "Steam didn't take it: " + (result.stderr or result.stdout).strip()
    return "Sent to Steam; it shows in your library (and Game Mode) as a non-Steam game"


# ---------------------------------------------------------------------------
# The launcher window.

STYLE = """
QWidget { color: #f3eee8; }
QToolTip { background: #1d1922; color: #f3eee8; border: 1px solid #ff8a1f; padding: 6px; }
#card { background: rgba(17, 14, 21, 228); border: 1px solid rgba(255, 138, 31, 70);
        border-radius: 20px; }
QTabWidget::pane { border: none; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: #a99f97; padding: 12px 18px 10px 18px;
               font-weight: 800; font-size: 13px; letter-spacing: 1px;
               border-bottom: 3px solid transparent; }
QTabBar::tab:selected { color: #ffffff; border-bottom: 3px solid #ff8a1f; }
QTabBar::tab:hover:!selected { color: #ffd2a8; }
QLabel#section { color: #ff9d45; font-weight: 800; font-size: 12px; letter-spacing: 2px; }
QLabel#hint { color: #9d948c; font-size: 12px; }
QLabel#chip { background: rgba(255, 255, 255, 18); border-radius: 8px; padding: 6px 10px;
              font-weight: 700; }
QLabel#chip[lit="true"] { background: #3fb6ff; color: #071521; }
QPushButton { background: rgba(255, 255, 255, 14); border: 1px solid rgba(255, 255, 255, 26);
              border-radius: 10px; padding: 8px 14px; font-weight: 700; }
QPushButton:hover { background: rgba(255, 138, 31, 46); }
QPushButton:focus { border: 2px solid #3fb6ff; }
QPushButton:disabled { color: #6c645e; }
QPushButton#seg:checked { background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                          stop:0 #ffa53a, stop:1 #f2581b); color: #1b0c03; border: none; }
QPushButton#key { min-width: 92px; font-family: "Noto Sans Mono"; }
QPushButton#key[capturing="true"] { border: 2px solid #3fb6ff; color: #3fb6ff;
                                    background: rgba(63, 182, 255, 30); }
QPushButton#key[empty="true"] { color: #6c645e; }
QComboBox { background: rgba(255, 255, 255, 14); border: 1px solid rgba(255, 255, 255, 26);
            border-radius: 10px; padding: 6px 12px; min-width: 150px; font-weight: 700; }
QComboBox:focus { border: 2px solid #3fb6ff; }
QComboBox[changed="true"] { color: #ffb066; border-color: rgba(255, 138, 31, 140); }
QComboBox QAbstractItemView { background: #1d1922; selection-background-color: #ff8a1f;
                              selection-color: #1b0c03; border: 1px solid #3a3340; }
QSlider::groove:horizontal { height: 6px; background: rgba(255, 255, 255, 30);
                             border-radius: 3px; }
QSlider::sub-page:horizontal { background: #ff8a1f; border-radius: 3px; }
QSlider::handle:horizontal { background: #fff4e6; width: 18px; margin: -7px 0;
                             border-radius: 9px; }
QCheckBox { font-weight: 700; spacing: 10px; }
QCheckBox::indicator { width: 20px; height: 20px; border-radius: 6px;
                       background: rgba(255, 255, 255, 20); border: 1px solid #5a5260; }
QCheckBox::indicator:checked { background: #ff8a1f; border: none; }
QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: rgba(255, 255, 255, 50); border-radius: 4px;
                              min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
"""


class PlayButton(QtWidgets.QAbstractButton):
    """A slanted orange PLAY button with a swirl."""

    def __init__(self):
        super().__init__()
        self.setCursor(Qt.PointingHandCursor)
        self.setFocusPolicy(Qt.StrongFocus)
        self.setFixedSize(300, 76)
        self.setText("PLAY")

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        w, h, slant = self.width(), self.height(), 22
        shape = QtGui.QPolygonF([QtCore.QPointF(slant, 2), QtCore.QPointF(w - 2, 2),
                                 QtCore.QPointF(w - slant, h - 2), QtCore.QPointF(2, h - 2)])
        gradient = QtGui.QLinearGradient(0, 0, w, h)
        if not self.isEnabled():
            gradient.setColorAt(0, QtGui.QColor("#5b5048"))
            gradient.setColorAt(1, QtGui.QColor("#3d3530"))
        else:
            lift = 25 if self.underMouse() or self.hasFocus() else 0
            gradient.setColorAt(0, QtGui.QColor("#ffb347").lighter(100 + lift))
            gradient.setColorAt(1, ORANGE_DEEP.lighter(100 + lift))
        painter.setPen(QtGui.QPen(CHAKRA, 3) if self.hasFocus() else Qt.NoPen)
        painter.setBrush(gradient)
        painter.drawPolygon(shape)
        painter.setPen(QtGui.QPen(INK, 4, Qt.SolidLine, Qt.RoundCap))
        painter.drawPath(spiral_path(QtCore.QPointF(58, h / 2), 2.2, 22))
        font = display_font(38)
        font.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, 6)
        painter.setFont(font)
        painter.setPen(INK)
        painter.drawText(QtCore.QRectF(70, 0, w - 90, h), Qt.AlignCenter, self.text())


class Segmented(QtWidgets.QWidget):
    """A row of exclusive choices."""

    changed = QtCore.Signal(str)

    def __init__(self, options, value):
        super().__init__()
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        self.group = QtWidgets.QButtonGroup(self)
        self.buttons = {}
        for key, label in options:
            button = QtWidgets.QPushButton(label)
            button.setObjectName("seg")
            button.setCheckable(True)
            button.setCursor(Qt.PointingHandCursor)
            button.setChecked(key == value)
            button.clicked.connect(lambda _=False, k=key: self.changed.emit(k))
            self.group.addButton(button)
            self.buttons[key] = button
            layout.addWidget(button)
        layout.addStretch()

    def set_value(self, value):
        if value in self.buttons:
            self.buttons[value].setChecked(True)


def section(text):
    label = QtWidgets.QLabel(text.upper())
    label.setObjectName("section")
    return label


def hint(text):
    label = QtWidgets.QLabel(text)
    label.setObjectName("hint")
    label.setWordWrap(True)
    return label


def scrolling(widget):
    area = QtWidgets.QScrollArea()
    area.setWidgetResizable(True)
    area.setWidget(widget)
    area.setFrameShape(QtWidgets.QFrame.NoFrame)
    return area


# Qt keys by the SDK's key names.
_QT_KEYS = {
    Qt.Key_QuoteLeft: "Backtick", Qt.Key_Minus: "Minus", Qt.Key_Equal: "Plus",
    Qt.Key_Comma: "Comma", Qt.Key_Period: "Period", Qt.Key_Semicolon: "Semicolon",
    Qt.Key_Slash: "Slash", Qt.Key_Backslash: "Backslash", Qt.Key_BracketLeft: "LBracket",
    Qt.Key_BracketRight: "RBracket", Qt.Key_Apostrophe: "Quote", Qt.Key_Return: "Return",
    Qt.Key_Enter: "Return", Qt.Key_Space: "Space", Qt.Key_Tab: "Tab", Qt.Key_Backtab: "Tab",
    Qt.Key_Backspace: "Backspace", Qt.Key_Delete: "Delete", Qt.Key_Insert: "Insert",
    Qt.Key_Home: "Home", Qt.Key_End: "End", Qt.Key_PageUp: "PageUp",
    Qt.Key_PageDown: "PageDown", Qt.Key_Left: "Left", Qt.Key_Right: "Right", Qt.Key_Up: "Up",
    Qt.Key_Down: "Down", Qt.Key_CapsLock: "CapsLock", Qt.Key_NumLock: "NumLock",
    Qt.Key_ScrollLock: "ScrollLock", Qt.Key_Pause: "Pause", Qt.Key_Print: "PrintScreen",
}
_QT_KEYPAD = {Qt.Key_Enter: "NumpadEnter", Qt.Key_Plus: "NumpadPlus",
              Qt.Key_Minus: "NumpadMinus", Qt.Key_Asterisk: "NumpadStar",
              Qt.Key_Slash: "NumpadSlash"}
_MODIFIER_KEYS = {Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_AltGr,
                  Qt.Key_Super_L, Qt.Key_Super_R}


def key_name(event):
    """The SDK's name for a pressed key, "" for a modifier, None if it has none."""
    key = event.key()
    if key in _MODIFIER_KEYS:
        return ""
    if event.modifiers() & Qt.KeypadModifier:
        if Qt.Key_0 <= key <= Qt.Key_9:
            return f"Numpad{key - Qt.Key_0}"
        if key in _QT_KEYPAD:
            return _QT_KEYPAD[key]
    if Qt.Key_A <= key <= Qt.Key_Z or Qt.Key_0 <= key <= Qt.Key_9:
        return chr(key)
    if Qt.Key_F1 <= key <= Qt.Key_F24:
        return f"F{key - Qt.Key_F1 + 1}"
    return _QT_KEYS.get(key)


class Launcher(QtWidgets.QWidget):
    def __init__(self):
        super().__init__()
        self.settings = load_settings()
        self.process = None
        self.capturing = None  # (action, slot, button)
        self.pads = sdl_pad.Pads()
        self.pad_held = set()
        self.background = load_art("background.jpg")
        self.setWindowTitle("Storm 3 Launcher")
        self.setMinimumSize(1060, 660)
        self.resize(1220, 720)
        self.setStyleSheet(STYLE)

        root = QtWidgets.QHBoxLayout(self)
        root.setContentsMargins(48, 40, 36, 36)
        root.setSpacing(36)
        root.addLayout(self.build_hero(), 5)
        card = QtWidgets.QFrame()
        card.setObjectName("card")
        card_layout = QtWidgets.QVBoxLayout(card)
        card_layout.setContentsMargins(18, 10, 18, 18)
        self.tabs = QtWidgets.QTabWidget()
        self.tabs.addTab(self.build_graphics(), "GRAPHICS")
        self.tabs.addTab(self.build_controller(), "CONTROLLER")
        self.tabs.addTab(self.build_keyboard(), "KEYBOARD")
        self.tabs.addTab(self.build_system(), "SYSTEM")
        card_layout.addWidget(self.tabs)
        root.addWidget(card, 6)

        QtWidgets.QApplication.instance().installEventFilter(self)
        self.pad_timer = QtCore.QTimer(self, interval=50, timeout=self.poll_pads)
        self.pad_timer.start()
        self.refresh_summary()
        self.refresh_ready()
        self.play.setFocus()

    # -- Left side ---------------------------------------------------------

    def build_hero(self):
        layout = QtWidgets.QVBoxLayout()
        layout.setSpacing(6)
        layout.addStretch(5)
        kicker = QtWidgets.QLabel("NARUTO SHIPPUDEN")
        kicker.setFont(display_font(22, QtGui.QFont.ExtraBold))
        kicker.setStyleSheet("color: #ff9d45; letter-spacing: 6px;")
        layout.addWidget(kicker)
        title = QtWidgets.QLabel("ULTIMATE NINJA\nSTORM 3")
        title.setFont(display_font(64))
        title.setStyleSheet("color: #fff7ee; line-height: 90%;")
        shadow = QtWidgets.QGraphicsDropShadowEffect(blurRadius=24, offset=QtCore.QPointF(0, 4))
        shadow.setColor(QtGui.QColor(0, 0, 0, 200))
        title.setGraphicsEffect(shadow)
        layout.addWidget(title)
        tagline = QtWidgets.QLabel("Recompiled  ·  native Vulkan renderer")
        tagline.setStyleSheet("color: #d9cfc6; font-weight: 600; letter-spacing: 1px;")
        layout.addWidget(tagline)
        layout.addSpacing(28)
        self.play = PlayButton()
        self.play.clicked.connect(self.start_game)
        layout.addWidget(self.play)
        layout.addSpacing(10)
        self.summary = QtWidgets.QLabel()
        self.summary.setStyleSheet("color: #ffd2a8; font-weight: 700;")
        layout.addWidget(self.summary)
        self.status = QtWidgets.QLabel()
        self.status.setWordWrap(True)
        self.status.setStyleSheet("color: #d9cfc6;")
        layout.addWidget(self.status)
        self.pad_status = QtWidgets.QLabel()
        self.pad_status.setStyleSheet("color: #9fd8ff;")
        layout.addWidget(self.pad_status)
        layout.addStretch(2)
        return layout

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setRenderHints(QtGui.QPainter.Antialiasing | QtGui.QPainter.SmoothPixmapTransform)
        rect = QtCore.QRectF(self.rect())
        painter.fillRect(rect, INK)
        if self.background:
            # Cover the window, a little closer in, with Naruto's face (at
            # 1000, 330 in the 1920x1080 key art) above the title.
            image = self.background
            art = image.width() / 1920
            scale = 1.25 * max(rect.width() / image.width(), rect.height() / image.height())
            w, h = rect.width() / scale, rect.height() / scale
            x = min(max(1000 * art - 0.27 * w, 0), image.width() - w)
            y = min(max(330 * art - 0.2 * h, 0), image.height() - h)
            painter.setOpacity(0.9)
            painter.drawImage(rect, image, QtCore.QRectF(x, y, w, h))
            painter.setOpacity(1)
        else:
            painter.setOpacity(0.25)
            draw_swirl_emblem(painter, QtCore.QRectF(-rect.height() * 0.3, rect.height() * 0.2,
                                                     rect.height(), rect.height()))
            painter.setOpacity(1)
        # Ink washes: down the left behind the title, and under everything.
        wash = QtGui.QLinearGradient(0, 0, rect.width(), 0)
        wash.setColorAt(0, QtGui.QColor(14, 10, 16, 200))
        wash.setColorAt(0.3, QtGui.QColor(14, 10, 16, 60))
        wash.setColorAt(0.5, QtGui.QColor(14, 10, 16, 90))
        wash.setColorAt(1, QtGui.QColor(14, 10, 16, 160))
        painter.fillRect(rect, wash)
        floor = QtGui.QLinearGradient(0, rect.height() * 0.42, 0, rect.height())
        floor.setColorAt(0, QtGui.QColor(14, 10, 16, 0))
        floor.setColorAt(0.35, QtGui.QColor(14, 10, 16, 190))
        floor.setColorAt(1, QtGui.QColor(14, 10, 16, 235))
        painter.fillRect(rect, floor)
        # A chakra swirl rising behind the title, and a brush slash.
        painter.setPen(QtGui.QPen(QtGui.QColor(255, 138, 31, 55), 3, Qt.SolidLine, Qt.RoundCap))
        painter.drawPath(spiral_path(QtCore.QPointF(rect.width() * 0.08, rect.height() * 0.5),
                                     3.2, rect.height() * 0.42))
        slash = QtGui.QPolygonF([QtCore.QPointF(0, rect.height() * 0.9),
                                 QtCore.QPointF(rect.width() * 0.42, rect.height() * 0.86),
                                 QtCore.QPointF(rect.width() * 0.4, rect.height() * 0.875),
                                 QtCore.QPointF(0, rect.height() * 0.925)])
        painter.setPen(Qt.NoPen)
        painter.setBrush(QtGui.QColor(255, 138, 31, 150))
        painter.drawPolygon(slash)

    # -- Graphics ----------------------------------------------------------

    def build_graphics(self):
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(8, 18, 8, 8)
        layout.setSpacing(8)
        s = self.settings

        def choice(title, options, key, note=None):
            layout.addWidget(section(title))
            row = Segmented(options, s[key])
            row.changed.connect(lambda value: self.set_setting(key, value))
            layout.addWidget(row)
            if note:
                layout.addWidget(note)
            layout.addSpacing(10)
            return row

        choice("Resolution", [(name, name) for name, _ in RESOLUTIONS], "resolution",
               hint("What the game renders at. 720p is the Xbox 360's own; higher is sharper "
                    "and heavier on the GPU."))
        display = Segmented([("window", "Windowed"), ("full", "Fullscreen")],
                            "full" if s["fullscreen"] else "window")
        display.changed.connect(lambda value: self.set_setting("fullscreen", value == "full"))
        layout.addWidget(section("Display"))
        layout.addWidget(display)
        layout.addWidget(hint("F11 switches between them in the game."))
        layout.addSpacing(10)
        choice("Screen ratio", ASPECTS, "aspect",
               hint("Pick your screen's shape. The game is made for 16:9; other shapes stretch "
                    "it to fill the screen. Fill screen matches whatever the window is."))
        self.upscaler_hint = hint(UPSCALER_HINTS[s["upscaler"]])
        choice("Upscaling to your screen", UPSCALERS, "upscaler", self.upscaler_hint)
        layout.addWidget(section("Anti-aliasing"))
        aa = Segmented([("off", "Off"), ("fxaa", "FXAA")], "fxaa" if s["fxaa"] else "off")
        aa.changed.connect(lambda value: self.set_setting("fxaa", value == "fxaa"))
        layout.addWidget(aa)
        layout.addWidget(hint("NVIDIA FXAA smooths jagged edges before upscaling. Softens a "
                              "little; pairs well with sharpening."))
        layout.addSpacing(10)
        layout.addWidget(section("Sharpness"))
        row = QtWidgets.QHBoxLayout()
        self.sharpness = QtWidgets.QSlider(Qt.Horizontal)
        self.sharpness.setRange(0, 100)
        self.sharpness.setValue(round(s["sharpness"] * 100))
        self.sharpness_value = QtWidgets.QLabel()
        self.sharpness_value.setFixedWidth(44)
        self.sharpness.valueChanged.connect(
            lambda value: self.set_setting("sharpness", value / 100))
        row.addWidget(self.sharpness)
        row.addWidget(self.sharpness_value)
        layout.addLayout(row)
        layout.addSpacing(10)
        gpus = [(key, label) for key, label, icd in GPUS if Path(icd).exists()]
        if len(gpus) > 1:
            choice("GPU", gpus, "gpu")
        layout.addStretch()
        return scrolling(page)

    # -- Controller --------------------------------------------------------

    def build_controller(self):
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(8, 18, 8, 8)
        layout.setSpacing(8)
        layout.addWidget(section("Controller type"))
        self.kind_row = Segmented(KINDS, self.settings["pad_kind"])
        self.kind_row.changed.connect(self.set_pad_kind)
        layout.addWidget(self.kind_row)
        layout.addWidget(hint("Each type keeps its own layout; the game picks it by the "
                              "controller's maker. Controllers through Steam Input count "
                              "as Steam / Other. Press a button to light up its row."))
        layout.addSpacing(6)
        presets = QtWidgets.QHBoxLayout()
        reset = QtWidgets.QPushButton("Default layout")
        reset.clicked.connect(lambda: self.apply_pad_preset({}))
        swap = QtWidgets.QPushButton("Swap A↔B and X↔Y")
        swap.setToolTip("Confirm with the right button: Nintendo labels, or the Japanese "
                        "PlayStation style")
        swap.clicked.connect(lambda: self.apply_pad_preset(
            {"A": "B", "B": "A", "X": "Y", "Y": "X"}))
        presets.addWidget(reset)
        presets.addWidget(swap)
        presets.addStretch()
        layout.addLayout(presets)
        layout.addSpacing(6)
        grid = QtWidgets.QGridLayout()
        grid.setHorizontalSpacing(14)
        grid.setVerticalSpacing(6)
        header_physical = section("Your button")
        header_game = section("Sends")
        grid.addWidget(header_physical, 0, 0)
        grid.addWidget(header_game, 0, 2)
        self.pad_chips = {}
        self.pad_combos = {}
        for row, button in enumerate(BUTTONS, start=1):
            chip = QtWidgets.QLabel()
            chip.setObjectName("chip")
            arrow = QtWidgets.QLabel("→")
            arrow.setStyleSheet("color: #ff9d45; font-weight: 800;")
            combo = QtWidgets.QComboBox()
            for target in BUTTONS + ["None"]:
                combo.addItem(OUTPUT_LABELS[target], target)
            combo.currentIndexChanged.connect(
                lambda _=0, b=button, c=combo: self.set_pad_target(b, c.currentData()))
            grid.addWidget(chip, row, 0)
            grid.addWidget(arrow, row, 1)
            grid.addWidget(combo, row, 2)
            self.pad_chips[button] = chip
            self.pad_combos[button] = combo
        grid.setColumnStretch(3, 1)
        layout.addLayout(grid)
        layout.addStretch()
        self.refresh_pad_rows()
        return scrolling(page)

    def set_pad_kind(self, kind):
        self.settings["pad_kind"] = kind
        self.refresh_pad_rows()
        self.persist()

    def refresh_pad_rows(self):
        kind = self.settings["pad_kind"]
        mapping = self.settings["pad_maps"].setdefault(kind, {})
        for button in BUTTONS:
            self.pad_chips[button].setText(PAD_LABELS[kind][button])
            combo = self.pad_combos[button]
            target = mapping.get(button, button)
            combo.blockSignals(True)
            combo.setCurrentIndex(combo.findData(target))
            combo.blockSignals(False)
            combo.setProperty("changed", target != button)
            combo.style().unpolish(combo)
            combo.style().polish(combo)

    def set_pad_target(self, button, target):
        mapping = self.settings["pad_maps"].setdefault(self.settings["pad_kind"], {})
        if target == button:
            mapping.pop(button, None)
        else:
            mapping[button] = target
        self.refresh_pad_rows()
        self.persist()

    def apply_pad_preset(self, preset):
        self.settings["pad_maps"][self.settings["pad_kind"]] = dict(preset)
        self.refresh_pad_rows()
        self.persist()

    # -- Keyboard ----------------------------------------------------------

    def build_keyboard(self):
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(8, 18, 8, 8)
        layout.setSpacing(8)
        enable = QtWidgets.QCheckBox("Play with the keyboard")
        enable.setChecked(self.settings["keyboard"])
        enable.toggled.connect(lambda on: self.set_setting("keyboard", on))
        layout.addWidget(enable)
        layout.addWidget(hint("Click a key box, then press the key for that Xbox control "
                              "(Esc cancels, right-click clears). A key used elsewhere moves "
                              "here. Shift, Ctrl and Alt can't be used on their own."))
        self.key_message = hint("")
        self.key_message.setStyleSheet("color: #9fd8ff;")
        layout.addWidget(self.key_message)
        grid = QtWidgets.QGridLayout()
        grid.setHorizontalSpacing(12)
        grid.setVerticalSpacing(6)
        self.key_buttons = {}
        row = 0
        for title, actions in KEY_GROUPS:
            grid.addWidget(section(title), row, 0, 1, 3)
            row += 1
            for name, label, _ in actions:
                grid.addWidget(QtWidgets.QLabel(label), row, 0)
                for slot in range(KEY_SLOTS):
                    button = QtWidgets.QPushButton()
                    button.setObjectName("key")
                    button.setCursor(Qt.PointingHandCursor)
                    button.setContextMenuPolicy(Qt.CustomContextMenu)
                    button.clicked.connect(
                        lambda _=False, n=name, s=slot, b=button: self.begin_capture(n, s, b))
                    button.customContextMenuRequested.connect(
                        lambda _=None, n=name, s=slot: self.assign_key(n, s, ""))
                    grid.addWidget(button, row, 1 + slot)
                    self.key_buttons[(name, slot)] = button
                row += 1
            grid.setRowMinimumHeight(row, 8)
            row += 1
        grid.setColumnStretch(3, 1)
        layout.addLayout(grid)
        reset = QtWidgets.QPushButton("Default keys")
        reset.clicked.connect(self.reset_keys)
        layout.addWidget(reset, 0, Qt.AlignLeft)
        layout.addStretch()
        self.refresh_keys()
        return scrolling(page)

    def refresh_keys(self):
        for (name, slot), button in self.key_buttons.items():
            keys = self.settings["keys"].get(name, [])
            key = keys[slot] if slot < len(keys) else ""
            capturing = self.capturing and self.capturing[2] is button
            button.setText("Press a key…" if capturing else key or "—")
            button.setProperty("capturing", bool(capturing))
            button.setProperty("empty", not key and not capturing)
            button.style().unpolish(button)
            button.style().polish(button)

    def begin_capture(self, name, slot, button):
        self.capturing = (name, slot, button)
        self.key_message.setText("")
        self.refresh_keys()

    def assign_key(self, name, slot, key):
        keys = self.settings["keys"]
        message = ""
        if key:
            for other, other_keys in keys.items():
                for index, other_key in enumerate(other_keys):
                    if other_key == key and (other, index) != (name, slot):
                        other_keys[index] = ""
                        if other != name:
                            message = f"{key} moved here from {KEY_LABELS[other]}. "
            if key in RESERVED_KEYS:
                message += f"Note: in the game {key} also {RESERVED_KEYS[key]}."
        slots = keys.setdefault(name, [])
        slots.extend([""] * (KEY_SLOTS - len(slots)))
        slots[slot] = key
        for other_keys in keys.values():
            while other_keys and not other_keys[-1]:
                other_keys.pop()
        self.capturing = None
        self.key_message.setText(message)
        self.refresh_keys()
        self.persist()

    def reset_keys(self):
        self.settings["keys"] = {name: list(keys) for name, _, keys in KEY_ACTIONS}
        self.key_message.setText("")
        self.refresh_keys()
        self.persist()

    def eventFilter(self, watched, event):
        if self.capturing and event.type() == QtCore.QEvent.KeyPress:
            if event.key() == Qt.Key_Escape:
                self.capturing = None
                self.refresh_keys()
                return True
            name = key_name(event)
            if name == "":
                self.key_message.setText("Shift, Ctrl and Alt can't be used on their own.")
                return True
            if name is None:
                self.key_message.setText("The game can't use that key.")
                return True
            self.assign_key(self.capturing[0], self.capturing[1], name)
            return True
        if self.capturing and event.type() == QtCore.QEvent.MouseButtonPress \
                and watched is not self.capturing[2]:
            self.capturing = None
            self.refresh_keys()
        return super().eventFilter(watched, event)

    # -- System ------------------------------------------------------------

    def build_system(self):
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(8, 18, 8, 8)
        layout.setSpacing(8)
        layout.addWidget(section("App menu"))
        layout.addWidget(hint("Adds the game (it plays with these settings) and this "
                              "launcher to your app menu, with icons made from your game's "
                              "own artwork."))
        install = QtWidgets.QPushButton("Add to the app menu")
        install.clicked.connect(self.install_entries)
        layout.addWidget(install, 0, Qt.AlignLeft)
        layout.addSpacing(10)
        layout.addWidget(section("Steam and Game Mode"))
        layout.addWidget(hint("Adds the game to Steam as a non-Steam game, so it shows in "
                              "Game Mode. Steam has to be running."))
        steam = QtWidgets.QPushButton("Add to Steam")
        steam.setEnabled(bool(shutil.which("steamos-add-to-steam")))
        steam.clicked.connect(lambda: self.system_message.setText(add_game_to_steam()))
        layout.addWidget(steam, 0, Qt.AlignLeft)
        layout.addSpacing(10)
        layout.addWidget(section("Folders"))
        folders = QtWidgets.QHBoxLayout()
        for label, path in (("Game folder", ROOT),
                            ("Logs", ROOT / "out" / "build" / "linux-amd64-release" / "logs"),
                            ("Launcher settings", CONFIG_FILE.parent)):
            button = QtWidgets.QPushButton(label)
            button.clicked.connect(lambda _=False, p=path: QtGui.QDesktopServices.openUrl(
                QtCore.QUrl.fromLocalFile(str(p))))
            folders.addWidget(button)
        folders.addStretch()
        layout.addLayout(folders)
        layout.addSpacing(10)
        self.system_message = hint("")
        self.system_message.setStyleSheet("color: #9fd8ff;")
        layout.addWidget(self.system_message)
        layout.addSpacing(10)
        layout.addWidget(section("In the game"))
        layout.addWidget(hint("F11  fullscreen or windowed\nF12  renderer snapshot (for "
                              "bug reports)\nClosing the window quits the game."))
        layout.addSpacing(10)
        layout.addWidget(section("Controller in this launcher"))
        layout.addWidget(hint("D-pad moves, A selects, LB / RB change tabs, Start plays."))
        layout.addStretch()
        return scrolling(page)

    def install_entries(self):
        self.system_message.setText("\n".join(install_menu_entries()))
        self.background = load_art("background.jpg")
        self.setWindowIcon(QtGui.QIcon(QtGui.QPixmap.fromImage(make_launcher_icon(256))))
        self.update()

    # -- Settings and the game ---------------------------------------------

    def set_setting(self, key, value):
        self.settings[key] = value
        if key == "upscaler":
            self.upscaler_hint.setText(UPSCALER_HINTS[value])
        if key == "gpu":
            self.refresh_ready()
        self.refresh_summary()
        self.persist()

    def persist(self):
        save_settings(self.settings)

    def refresh_summary(self):
        s = self.settings
        upscaler = dict(UPSCALERS)[s["upscaler"]]
        parts = [s["resolution"], dict(ASPECTS)[s["aspect"]],
                 "Fullscreen" if s["fullscreen"] else "Windowed", upscaler]
        if s["fxaa"]:
            parts.append("FXAA")
        if s["keyboard"]:
            parts.append("Keyboard on")
        self.summary.setText("   ·   ".join(parts))
        self.sharpness_value.setText(f"{round(s['sharpness'] * 100)}%")
        self.sharpness.setEnabled(s["upscaler"] != "linear")

    def refresh_ready(self):
        problem = check_game(self.settings)
        self.play.setEnabled(problem is None and self.process is None)
        self.status.setText("Ready." if problem is None else problem)

    def start_game(self):
        if self.process:
            return
        command, env = game_command(self.settings)
        LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        log = open(LOG_FILE, "w")
        self.process = subprocess.Popen(command, env=env, cwd=ROOT, stdout=log,
                                        stderr=subprocess.STDOUT)
        log.close()
        self.play.setEnabled(False)
        self.status.setText("Playing…")
        self.showMinimized()
        self.game_timer = QtCore.QTimer(self, interval=500, timeout=self.watch_game)
        self.game_timer.start()

    def watch_game(self):
        code = self.process.poll()
        if code is None:
            return
        self.game_timer.stop()
        self.process = None
        self.refresh_ready()
        if code != 0:
            self.status.setText(f"The game stopped with code {code}; see {LOG_FILE}.")
        self.showNormal()
        self.activateWindow()

    # -- Controllers ---------------------------------------------------------

    def poll_pads(self):
        pads = self.pads.poll()
        if not self.pads.available:
            self.pad_status.setText("Controllers: SDL 3 not found")
        elif not pads:
            self.pad_status.setText("No controller connected")
        else:
            names = [f"{name} ({dict(KINDS)[kind]})" for name, kind, _ in pads]
            self.pad_status.setText("🎮  " + ",  ".join(names))
        held = set().union(*(buttons for _, _, buttons in pads)) if pads else set()
        pressed = held - self.pad_held
        self.pad_held = held
        # Light the rows of the buttons held on a controller of the shown type.
        kind = self.settings["pad_kind"]
        lit = set().union(*(b for _, k, b in pads if k == kind)) if pads else set()
        for button, chip in self.pad_chips.items():
            if chip.property("lit") != (button in lit):
                chip.setProperty("lit", button in lit)
                chip.style().unpolish(chip)
                chip.style().polish(chip)
        if self.isActiveWindow() and pressed:
            self.navigate(pressed)

    def navigate(self, pressed):
        """Moves around the launcher with a controller."""
        focus = QtWidgets.QApplication.focusWidget()
        if "Start" in pressed and self.play.isEnabled():
            self.play.click()
        elif pressed & {"LB", "RB"}:
            step = -1 if "LB" in pressed else 1
            self.tabs.setCurrentIndex((self.tabs.currentIndex() + step) % self.tabs.count())
        elif pressed & {"Down", "Right", "StickDown"}:
            self.focusNextChild()
        elif pressed & {"Up", "Left", "StickUp"}:
            self.focusPreviousChild()
        elif "A" in pressed and focus:
            if isinstance(focus, QtWidgets.QComboBox):
                focus.showPopup()
            elif isinstance(focus, QtWidgets.QAbstractButton):
                focus.click()
        elif "B" in pressed:
            self.play.setFocus()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--play", action="store_true", help="start the game right away")
    parser.add_argument("--install", action="store_true", help="add the app menu entries")
    parser.add_argument("--steam", action="store_true",
                        help="with --install, also add the game to Steam")
    parser.add_argument("--android-icons", metavar="RES_DIR",
                        help="write the Android app icon, made from the game's art, "
                             "into RES_DIR/mipmap-*/ic_launcher.png")
    args = parser.parse_args()
    if args.android_icons:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        app = QtGui.QGuiApplication(sys.argv)
        if not extract_art():
            print("no game art in assets/nxeart; keeping the default icon")
            return
        for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                              ("xxhdpi", 144), ("xxxhdpi", 192)):
            folder = Path(args.android_icons) / f"mipmap-{density}"
            folder.mkdir(parents=True, exist_ok=True)
            make_game_icon(size).save(str(folder / "ic_launcher.png"))
        print(f"wrote the app icon to {args.android_icons}")
        return
    if args.play:
        play_now()
    if args.install:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        app = QtGui.QGuiApplication(sys.argv)
        for line in install_menu_entries(add_to_steam=args.steam):
            print(line)
        return
    QtWidgets.QApplication.setDesktopFileName(LAUNCHER_ID)
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("Storm 3 Launcher")
    font = QtGui.QFont("Noto Sans")
    font.setPixelSize(14)
    app.setFont(font)
    app.setWindowIcon(QtGui.QIcon(QtGui.QPixmap.fromImage(make_launcher_icon(256))))
    window = Launcher()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
