"""
GBPy Qt GUI

PyQt6-based frontend for the gbpy emulator.
- Renders framebuffer via QImage
- Plays audio via QAudioOutput
- Maps keyboard to emulator buttons
"""

import sys
import numpy as np
from PyQt6.QtWidgets import QApplication, QMainWindow, QFileDialog, QMenuBar
from PyQt6.QtGui import QImage, QPixmap, QKeyEvent, QAction
from PyQt6.QtCore import QTimer, Qt, QByteArray, QIODevice
from PyQt6.QtMultimedia import QAudioFormat, QAudioSink

import gbpy
from gbpy import Emulator


# Key → Button mapping
KEY_MAP = {
    Qt.Key.Key_Z:      gbpy.BTN_A,
    Qt.Key.Key_X:      gbpy.BTN_B,
    Qt.Key.Key_Return: gbpy.BTN_START,
    Qt.Key.Key_Backspace: gbpy.BTN_SELECT,
    Qt.Key.Key_Up:     gbpy.BTN_UP,
    Qt.Key.Key_Down:   gbpy.BTN_DOWN,
    Qt.Key.Key_Left:   gbpy.BTN_LEFT,
    Qt.Key.Key_Right:  gbpy.BTN_RIGHT,
    Qt.Key.Key_A:      gbpy.BTN_L,
    Qt.Key.Key_S:      gbpy.BTN_R,
}

SCALE = 3  # Display scale factor


class AudioPlayer:
    """Manages audio output via Qt multimedia."""

    def __init__(self):
        fmt = QAudioFormat()
        fmt.setSampleRate(gbpy.AUDIO_SAMPLE_RATE)
        fmt.setChannelCount(2)
        fmt.setSampleFormat(QAudioFormat.SampleFormat.Int16)

        self.sink = QAudioSink(fmt)
        self.sink.setBufferSize(8192)
        self.io = self.sink.start()

    def push(self, data: bytes):
        if self.io and len(data) > 0:
            self.io.write(data)

    def stop(self):
        self.sink.stop()


class EmulatorWindow(QMainWindow):
    """Main emulator window with framebuffer display and input handling."""

    def __init__(self):
        super().__init__()
        self.emu = Emulator()
        self.audio = None
        self.timer = QTimer()
        self.timer.timeout.connect(self._tick)

        self._init_ui()

    def _init_ui(self):
        self.setWindowTitle("GBPy Emulator")
        self.setFixedSize(240 * SCALE, 160 * SCALE)  # Default GBA size

        # Menu bar
        menubar = self.menuBar()
        file_menu = menubar.addMenu("File")

        open_action = QAction("Open ROM...", self)
        open_action.setShortcut("Ctrl+O")
        open_action.triggered.connect(self._open_rom)
        file_menu.addAction(open_action)

        file_menu.addSeparator()

        save_state_action = QAction("Save State", self)
        save_state_action.setShortcut("F5")
        save_state_action.triggered.connect(self._save_state)
        file_menu.addAction(save_state_action)

        load_state_action = QAction("Load State", self)
        load_state_action.setShortcut("F7")
        load_state_action.triggered.connect(self._load_state)
        file_menu.addAction(load_state_action)

        file_menu.addSeparator()

        reset_action = QAction("Reset", self)
        reset_action.setShortcut("Ctrl+R")
        reset_action.triggered.connect(self._reset)
        file_menu.addAction(reset_action)

        quit_action = QAction("Quit", self)
        quit_action.setShortcut("Ctrl+Q")
        quit_action.triggered.connect(self.close)
        file_menu.addAction(quit_action)

        self._rom_path = None
        self._state_path = None

    def _open_rom(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Open ROM",
            "",
            "ROM Files (*.gb *.gbc *.gba);;All Files (*)"
        )
        if path:
            self._load_rom(path)

    def _load_rom(self, path: str):
        try:
            self.emu.load_rom_file(path)
        except RuntimeError as e:
            self.setWindowTitle(f"GBPy - Error: {e}")
            return

        self._rom_path = path

        # Resize window based on mode
        w = self.emu.screen_width
        h = self.emu.screen_height
        self.setFixedSize(w * SCALE, h * SCALE + self.menuBar().height())
        self.setWindowTitle(f"GBPy - {path.split('/')[-1]} [{self.emu.mode}]")

        # State file path (same dir as ROM, .state extension)
        self._state_path = path.rsplit(".", 1)[0] + ".state"

        # Start audio
        if self.audio:
            self.audio.stop()
        self.audio = AudioPlayer()

        # Start emulation at ~60 FPS
        self.timer.start(16)

    def _tick(self):
        """Run one frame and update display."""
        if not self.emu.running:
            return

        self.emu.run_frame()

        # Get framebuffer and display
        fb_data = self.emu.get_framebuffer()
        if fb_data:
            w = self.emu.screen_width
            h = self.emu.screen_height
            img = QImage(fb_data, w, h, w * 4, QImage.Format.Format_RGBA8888)
            scaled = img.scaled(w * SCALE, h * SCALE, Qt.AspectRatioMode.IgnoreAspectRatio,
                               Qt.TransformationMode.FastTransformation)
            pixmap = QPixmap.fromImage(scaled)
            # Paint to window
            self._pixmap = pixmap
            self.update()

        # Push audio
        if self.audio:
            audio_data = self.emu.get_audio()
            if audio_data:
                self.audio.push(audio_data)

    def paintEvent(self, event):
        if hasattr(self, '_pixmap'):
            from PyQt6.QtGui import QPainter
            painter = QPainter(self)
            y_offset = self.menuBar().height()
            painter.drawPixmap(0, y_offset, self._pixmap)
            painter.end()

    def keyPressEvent(self, event: QKeyEvent):
        if event.isAutoRepeat():
            return
        key = event.key()
        if key in KEY_MAP:
            self.emu.button_press(KEY_MAP[key])
        else:
            super().keyPressEvent(event)

    def keyReleaseEvent(self, event: QKeyEvent):
        if event.isAutoRepeat():
            return
        key = event.key()
        if key in KEY_MAP:
            self.emu.button_release(KEY_MAP[key])
        else:
            super().keyReleaseEvent(event)

    def _save_state(self):
        if self._state_path and self.emu.running:
            try:
                self.emu.save_state(self._state_path)
                self.setWindowTitle(self.windowTitle().split(" [Saved]")[0] + " [Saved]")
            except RuntimeError:
                pass

    def _load_state(self):
        if self._state_path and self.emu.running:
            try:
                self.emu.load_state(self._state_path)
                self.setWindowTitle(self.windowTitle().split(" [Loaded]")[0] + " [Loaded]")
            except RuntimeError:
                pass

    def _reset(self):
        if self.emu.running:
            self.emu.reset()

    def closeEvent(self, event):
        self.timer.stop()
        if self.audio:
            self.audio.stop()
        if self.emu.running:
            self.emu.save_ram()
        event.accept()


def run():
    """Launch the emulator GUI."""
    app = QApplication(sys.argv)
    window = EmulatorWindow()
    window.show()

    # If a ROM path was passed as argument, load it
    if len(sys.argv) > 1:
        window._load_rom(sys.argv[1])

    sys.exit(app.exec())
