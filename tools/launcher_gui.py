"""Window front end for tools/launcher.py (needs PySide6: pip install PySide6)."""

from pathlib import Path
import subprocess

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFileDialog, QFormLayout, QHBoxLayout,
    QLabel, QLineEdit, QMessageBox, QPushButton, QVBoxLayout, QWidget)

import launcher

RESOLUTION_HELP = ("Window size. The image is scaled to it by the upscaler, so this costs\n"
                   "almost nothing. In fullscreen the image fills the screen instead.")
RENDER_SCALE_HELP = ("How many pixels the game is drawn at. 1x is the original 1280x720.\n"
                     "2x is sharper but takes about 4x the GPU work (an RTX 2050 drops from\n"
                     "30 to ~18 FPS). Only whole numbers are supported.")
UPSCALER_HELP = ("Sharpens the image as it is scaled up to your screen, at almost no cost.\n"
                 "FSR 1 is the sharpest. Needs an SDK set up by tools/setup-sdk.")
KEYBOARD_HELP = ("W A S D: left stick   Arrows: right stick   Space: A   Backspace: B\n"
                 "L: X   P: Y   Q / E: triggers   1 / 3: shoulders   Enter: Start   Tab: Back")


class LauncherWindow(QWidget):
    def __init__(self, settings):
        super().__init__()
        self.settings = settings
        self.game = None
        self.setWindowTitle("Naruto Launcher")
        self.setMinimumWidth(560)

        self.xex = QLineEdit(settings["xex"])
        self.xex.editingFinished.connect(self.xex_changed)
        browse = QPushButton("Browse…")
        browse.clicked.connect(self.browse)
        xex_row = QHBoxLayout()
        xex_row.addWidget(self.xex)
        xex_row.addWidget(browse)
        self.xex_status = QLabel()

        self.gpu = QComboBox()
        choices, found = launcher.gpu_choices()
        for gpu_id, label in choices:
            self.gpu.addItem(label, gpu_id)
        index = self.gpu.findData(settings["gpu"])
        self.gpu.setCurrentIndex(max(index, 0))
        self.gpu.currentIndexChanged.connect(self.changed)

        self.keyboard = QCheckBox("Use keyboard controls")
        self.keyboard.setChecked(settings["keyboard"])
        self.keyboard.setToolTip(KEYBOARD_HELP)
        self.keyboard.toggled.connect(self.changed)

        self.resolution = QComboBox()
        for key, (label, _) in launcher.RESOLUTIONS.items():
            self.resolution.addItem(label, key)
        self.resolution.setCurrentIndex(max(self.resolution.findData(settings["resolution"]), 0))
        self.resolution.setToolTip(RESOLUTION_HELP)
        self.resolution.currentIndexChanged.connect(self.changed)
        self.fullscreen = QCheckBox("Fullscreen")
        self.fullscreen.setChecked(settings["fullscreen"])
        self.fullscreen.toggled.connect(self.changed)
        resolution_row = QHBoxLayout()
        resolution_row.addWidget(self.resolution, 1)
        resolution_row.addWidget(self.fullscreen)

        self.render_scale = QComboBox()
        for key, label in launcher.RENDER_SCALES.items():
            self.render_scale.addItem(label, key)
        self.render_scale.setCurrentIndex(max(self.render_scale.findData(settings["render_scale"]), 0))
        self.render_scale.setToolTip(RENDER_SCALE_HELP)
        self.render_scale.currentIndexChanged.connect(self.changed)

        self.upscaler = QComboBox()
        for key, label in launcher.UPSCALERS.items():
            self.upscaler.addItem(label, key)
        self.upscaler.setCurrentIndex(max(self.upscaler.findData(settings["upscaler"]), 0))
        self.upscaler.setToolTip(UPSCALER_HELP)
        self.upscaler.currentIndexChanged.connect(self.changed)

        self.build = QComboBox()
        self.build.addItem("Release", "release")
        self.build.addItem("Debug", "debug")
        self.build.setCurrentIndex(max(self.build.findData(settings["build"]), 0))
        self.build.currentIndexChanged.connect(self.changed)

        form = QFormLayout()
        form.addRow("Game (default.xex):", xex_row)
        form.addRow("", self.xex_status)
        form.addRow("GPU:", self.gpu)
        if found:
            form.addRow("", QLabel(f"GPUs in this PC: {found}"))
        form.addRow("Resolution:", resolution_row)
        form.addRow("Render scale:", self.render_scale)
        form.addRow("Upscaler:", self.upscaler)
        form.addRow("Controls:", self.keyboard)
        form.addRow("Build:", self.build)

        self.play = QPushButton("Play")
        self.play.setMinimumHeight(44)
        self.play.clicked.connect(self.start)
        self.stop = QPushButton("Stop game")
        self.stop.setMinimumHeight(44)
        self.stop.setEnabled(False)
        self.stop.clicked.connect(self.stop_game)
        buttons = QHBoxLayout()
        buttons.addWidget(self.play, 2)
        buttons.addWidget(self.stop, 1)
        self.status = QLabel("Ready.")

        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addLayout(buttons)
        layout.addWidget(self.status)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.xex_changed()

    def changed(self):
        self.settings["gpu"] = self.gpu.currentData()
        self.settings["keyboard"] = self.keyboard.isChecked()
        self.settings["build"] = self.build.currentData()
        self.settings["resolution"] = self.resolution.currentData()
        self.settings["render_scale"] = self.render_scale.currentData()
        self.settings["fullscreen"] = self.fullscreen.isChecked()
        self.settings["upscaler"] = self.upscaler.currentData()
        launcher.save_settings(self.settings)

    def xex_changed(self):
        text = self.xex.text().strip()
        if text and Path(text).is_file():
            text = str(Path(text).resolve())
            self.xex.setText(text)
        self.settings["xex"] = text
        problem = launcher.xex_status(self.settings["xex"])
        if problem:
            self.xex_status.setText(f"✗ {problem}")
            self.xex_status.setStyleSheet("color: #c0392b")
        else:
            self.xex_status.setText("✓ Game files OK")
            self.xex_status.setStyleSheet("color: #27ae60")
        self.changed()

    def browse(self):
        start = str(Path(self.xex.text()).parent)
        path, _ = QFileDialog.getOpenFileName(
            self, "Select default.xex", start, "Xbox 360 executable (default.xex);;All files (*)")
        if path:
            self.xex.setText(path)
            self.xex_changed()

    def start(self):
        self.xex_changed()
        try:
            command, env = launcher.prepare_launch(self.settings)
        except launcher.LaunchError as error:
            QMessageBox.warning(self, "Cannot launch", str(error))
            return
        self.memory_files = launcher.guest_memory_files()
        self.game = subprocess.Popen(command, cwd=launcher.ROOT, env=env)
        self.play.setEnabled(False)
        self.stop.setEnabled(True)
        self.status.setText(f"Running on {self.gpu.currentText()}…")
        self.timer.start(1000)

    def stop_game(self):
        """Ask the game to quit; force it if it is still running 5 s later."""
        if self.game and self.game.poll() is None:
            self.status.setText("Stopping the game…")
            self.game.terminate()
            game = self.game
            QTimer.singleShot(5000, lambda: game.poll() is None and game.kill())

    def poll(self):
        code = self.game.poll()
        if code is None:
            return
        self.timer.stop()
        self.game = None
        launcher.remove_guest_memory_files(self.memory_files)
        self.play.setEnabled(True)
        self.stop.setEnabled(False)
        self.status.setText("Game closed." if code == 0 else f"Game exited with code {code}; see its logs folder.")


def run(settings):
    app = QApplication.instance() or QApplication([])
    window = LauncherWindow(settings)
    window.show()
    return app.exec()
