#!python3
# -*- coding: utf-8 -*-
"""PyQt5 demo for ALSABridge: capture / playback with live waveform."""

import os
import sys
import wave
from pathlib import Path

from PyQt5.QtCore import QObject, QPointF, Qt, QThread, QTimer, pyqtSignal
from PyQt5.QtGui import QColor, QPainter, QPen
from PyQt5.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

import alsabridge

# ---------------------------------------------------------------------------
# Waveform helpers
# ---------------------------------------------------------------------------

UI_BG_PANEL = "#f0f4fa"
BAR_COLOR_CAPTURE = "#4a90d9"
BAR_COLOR_PLAYBACK = "#4a7abf"
PLAYHEAD_COLOR = "#c0392b"


def pcm_to_peaks(pcm_data: bytes, channels: int, bits_per_sample: int,
                 target_bars: int, absolute: bool = False) -> list[float]:
    """Convert raw PCM bytes to a normalised peak list (0.0–1.0).

    If *absolute* is True, normalise against the maximum possible sample
    value (32767 for 16-bit) so quiet audio produces small bars.
    Otherwise normalise against the loudest bar in the current data.
    """
    if not pcm_data or target_bars < 1:
        return []
    bytes_per_sample = bits_per_sample // 8
    frame_bytes = channels * bytes_per_sample
    if frame_bytes <= 0 or len(pcm_data) < frame_bytes:
        return []

    # To mono magnitudes (int16 scale)
    mono: list[int] = []
    for i in range(0, len(pcm_data) - frame_bytes + 1, frame_bytes):
        vals = []
        for c in range(channels):
            off = i + c * bytes_per_sample
            raw = pcm_data[off:off + bytes_per_sample]
            if bits_per_sample == 8:
                vals.append(raw[0] - 128)
            else:
                vals.append(int.from_bytes(raw, byteorder='little', signed=True))
        mono.append(abs(sum(vals) // channels))

    if not mono:
        return []

    n = len(mono)
    chunk = max(1, n // target_bars)
    mags = [0] * min(target_bars, (n + chunk - 1) // chunk)
    for idx, i in enumerate(range(0, n, chunk)):
        if idx >= len(mags):
            break
        mags[idx] = max(mono[i:i + chunk])

    # Normalise
    max_val = (1 << (bits_per_sample - 1)) - 1 if absolute else max(mags)
    if max_val < 1:
        max_val = 1
    return [min(m / max_val, 1.0) for m in mags]


# ---------------------------------------------------------------------------
# WaveformWidget
# ---------------------------------------------------------------------------

class WaveformWidget(QWidget):
    """Draws bar-style waveform with optional playhead."""

    seek_requested = pyqtSignal(float)

    def __init__(self, bar_color: str = BAR_COLOR_CAPTURE,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self._peaks: list[float] = []
        self._playhead: float = 0.0
        self._bar_color = QColor(bar_color)
        self.setMinimumHeight(60)
        self.setMinimumWidth(200)

    def set_bar_color(self, color: str) -> None:
        self._bar_color = QColor(color)

    def set_peaks(self, peaks: list[float]) -> None:
        self._peaks = list(peaks)
        self.update()

    def clear(self) -> None:
        self._peaks = []
        self._playhead = 0.0
        self.update()

    def set_playhead(self, ratio: float) -> None:
        r = max(0.0, min(1.0, ratio))
        if abs(r - self._playhead) > 1e-4:
            self._playhead = r
            self.update()

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.LeftButton and self._peaks:
            margin = 2
            w = self.width() - 2 * margin
            if w > 0:
                ratio = max(0.0, min(1.0, (event.pos().x() - margin) / w))
                self.seek_requested.emit(ratio)
        super().mousePressEvent(event)

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        rect = self.rect()
        p.fillRect(rect, QColor(UI_BG_PANEL))
        p.setPen(QPen(QColor("#c8d4e6"), 1))
        p.drawRect(rect.adjusted(0, 0, -1, -1))

        margin = 2
        inner = rect.adjusted(margin, margin, -margin, -margin)
        w, h = inner.width(), inner.height()
        if h <= 0 or w <= 0 or not self._peaks:
            p.end()
            return

        mid = inner.top() + h / 2.0
        half = h / 2.0
        n = len(self._peaks)
        il = inner.left()

        p.setPen(Qt.NoPen)
        p.setBrush(self._bar_color)
        for i, pk in enumerate(self._peaks):
            x0 = int(il + round(i * w / n))
            x1 = int(il + round((i + 1) * w / n))
            bw = x1 - x0
            if bw < 1:
                bw = 1
            ph = max(1.0, pk * half)
            p.fillRect(x0, int(mid - ph), bw, int(2 * ph), self._bar_color)

        # Playhead
        xh = inner.left() + self._playhead * w
        p.setPen(QPen(QColor(PLAYHEAD_COLOR), 2))
        p.drawLine(QPointF(xh, inner.top()), QPointF(xh, inner.bottom()))
        p.end()


# ---------------------------------------------------------------------------
# Playback (uses alsabridge.WavPlaybackImpl)
# ---------------------------------------------------------------------------

class PlaybackSignals(QObject):
    progress = pyqtSignal(float)   # playhead_ratio


class _PlaybackRunner(QThread):
    """Runs the blocking playback flow in a background thread."""

    playback_done = pyqtSignal()
    error = pyqtSignal(str)

    def __init__(self, dev: alsabridge.AlsaPlaybackDevice,
                 cb: alsabridge.WavPlaybackImpl, device_id: str, parent=None):
        super().__init__(parent)
        self.dev = dev
        self.cb = cb
        self.device_id = device_id

    def run(self) -> None:
        try:
            if not self.dev.open(self.device_id):
                self.error.emit('Failed to open playback device')
                return
            self.dev.set_playback_callback(self.cb)
            self.dev.set_min_cache_period_count(2)
            self.dev.set_period(period_count=5, period_time=40)
            sr = self.cb.sample_rate
            ch = self.cb.channels
            bits = self.cb.bits_per_sample
            if not self.dev.set_params(sr, ch, bits):
                self.error.emit('set_params failed')
                return
            if not self.dev.start():
                self.error.emit('start playback failed')
                return
            self.cb.should_stop_event.wait()
            self.dev.async_stop()
            self.cb.stopped_event.wait()
            self.dev.stop()
            self.playback_done.emit()
        except Exception as e:
            self.error.emit(str(e))


# ---------------------------------------------------------------------------
# MainWindow
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    DEFAULT_SR = 16000
    DEFAULT_CH = 2
    DEFAULT_BITS = 16

    def __init__(self):
        super().__init__()
        self.setWindowTitle('ALSABridge Demo')
        self.resize(800, 500)

        self._capture_dev: alsabridge.AlsaCaptureDevice | None = None
        self._playback_dev: alsabridge.AlsaPlaybackDevice | None = None
        self._capture_cb: alsabridge.CaptureBufferCallback | None = None
        self._playback_cb: alsabridge.WavPlaybackImpl | None = None
        self._playback_reader: alsabridge.WavReader | None = None
        self._playback_worker: _PlaybackRunner | None = None
        self._playback_signals: PlaybackSignals | None = None
        self._pcm_full_buffer = bytearray()
        self._capture_timer: QTimer | None = None
        self._is_capturing = False
        self._is_playing = False

        self._build_ui()
        self._refresh_devices()

    # ---- UI construction ---------------------------------------------------

    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(8, 8, 8, 8)

        # --- Device group ---
        dev_grp = QGroupBox('Devices')
        dev_lay = QVBoxLayout(dev_grp)

        row1 = QHBoxLayout()
        row1.addWidget(QLabel('Capture:'))
        self.combo_capture = QComboBox()
        self.combo_capture.setMinimumWidth(260)
        row1.addWidget(self.combo_capture, 1)
        self.btn_refresh = QPushButton('Refresh')
        self.btn_refresh.clicked.connect(self._refresh_devices)
        row1.addWidget(self.btn_refresh)
        dev_lay.addLayout(row1)

        row2 = QHBoxLayout()
        row2.addWidget(QLabel('Playback:'))
        self.combo_playback = QComboBox()
        self.combo_playback.setMinimumWidth(260)
        row2.addWidget(self.combo_playback, 1)
        row2.addStretch()
        dev_lay.addLayout(row2)

        row3 = QHBoxLayout()
        row3.addWidget(QLabel('Sample Rate:'))
        self.spin_sr = QSpinBox()
        self.spin_sr.setRange(8000, 192000)
        self.spin_sr.setValue(self.DEFAULT_SR)
        self.spin_sr.setSuffix(' Hz')
        row3.addWidget(self.spin_sr)
        row3.addWidget(QLabel('Channels:'))
        self.spin_ch = QSpinBox()
        self.spin_ch.setRange(1, 8)
        self.spin_ch.setValue(self.DEFAULT_CH)
        row3.addWidget(self.spin_ch)
        row3.addWidget(QLabel('Bits:'))
        self.combo_bits = QComboBox()
        for bits in (8, 16, 24, 32):
            self.combo_bits.addItem(str(bits), bits)
        self.combo_bits.setCurrentText(str(self.DEFAULT_BITS))
        row3.addWidget(self.combo_bits)
        row3.addStretch()
        dev_lay.addLayout(row3)

        root.addWidget(dev_grp)

        # --- Capture group ---
        cap_grp = QGroupBox('Capture')
        cap_lay = QVBoxLayout(cap_grp)

        cap_ctrl = QHBoxLayout()
        self.btn_capture = QPushButton('Start Capture')
        self.btn_capture.clicked.connect(self._toggle_capture)
        cap_ctrl.addWidget(self.btn_capture)
        cap_ctrl.addWidget(QLabel('Display seconds:'))
        self.spin_display_sec = QSpinBox()
        self.spin_display_sec.setRange(1, 60)
        self.spin_display_sec.setValue(5)
        self.spin_display_sec.setSuffix(' s')
        cap_ctrl.addWidget(self.spin_display_sec)
        cap_ctrl.addWidget(QLabel('Save as:'))
        self.edit_save_file = QLineEdit('cap.wav')
        self.edit_save_file.setMinimumWidth(160)
        cap_ctrl.addWidget(self.edit_save_file)
        cap_ctrl.addStretch()
        cap_lay.addLayout(cap_ctrl)

        self.waveform_capture = WaveformWidget(BAR_COLOR_CAPTURE)
        cap_lay.addWidget(self.waveform_capture)

        root.addWidget(cap_grp)

        # --- Playback group ---
        play_grp = QGroupBox('Playback')
        play_lay = QVBoxLayout(play_grp)

        play_ctrl = QHBoxLayout()
        self.btn_browse = QPushButton('Browse WAV ...')
        self.btn_browse.clicked.connect(self._browse_wav)
        play_ctrl.addWidget(self.btn_browse)
        self.lbl_file = QLabel('No file selected')
        play_ctrl.addWidget(self.lbl_file, 1)
        self.btn_play = QPushButton('Play')
        self.btn_play.clicked.connect(self._toggle_playback)
        play_ctrl.addWidget(self.btn_play)
        play_ctrl.addStretch()
        play_lay.addLayout(play_ctrl)

        self.waveform_playback = WaveformWidget(BAR_COLOR_PLAYBACK)
        self.waveform_playback.seek_requested.connect(self._on_seek_requested)
        play_lay.addWidget(self.waveform_playback)

        root.addWidget(play_grp)

        self._wav_path: str | None = None

    # ---- Device enumeration ------------------------------------------------

    def _refresh_devices(self) -> None:
        self.combo_capture.clear()
        for dev in alsabridge.get_capture_devices():
            self.combo_capture.addItem(dev.device_id)

        self.combo_playback.clear()
        for dev in alsabridge.get_playback_devices():
            self.combo_playback.addItem(dev.device_id)

    # ---- Capture ------------------------------------------------------------

    def _toggle_capture(self) -> None:
        if self._is_capturing:
            self._stop_capture()
        else:
            self._start_capture()

    def _start_capture(self) -> None:
        self._pcm_full_buffer = bytearray()
        self.waveform_capture.clear()
        self.waveform_capture.set_playhead(1.0)

        self._capture_dev = alsabridge.AlsaCaptureDevice()
        dev = self._capture_dev
        device_id = self.combo_capture.currentText() or 'default'

        if not dev.open(device_id):
            QMessageBox.warning(
                self, 'Capture',
                f"Failed to open capture device '{device_id}'.")
            self._release_capture()
            return
        sr = self.spin_sr.value()
        ch = self.spin_ch.value()
        bits = self._selected_bits()
        if not dev.set_period(period_count=5, period_time=40):
            QMessageBox.warning(self, 'Capture', 'set_period failed')
            self._release_capture()
            return
        if not dev.set_params(sr, ch, bits):
            QMessageBox.warning(
                self, 'Capture',
                f'set_params failed ({sr} Hz, {ch} ch, {bits} bit).')
            self._release_capture()
            return

        self._capture_cb = alsabridge.CaptureBufferCallback(self._pcm_full_buffer)
        dev.set_capture_callback(self._capture_cb)

        self._is_capturing = True
        self.btn_capture.setText('Stop Capture')

        self._capture_timer = QTimer(self)
        self._capture_timer.timeout.connect(self._update_capture_waveform)
        self._capture_timer.start(100)

        dev.start()

    def _stop_capture(self) -> None:
        if self._capture_dev:
            self._capture_dev.stop()
        if self._capture_timer:
            self._capture_timer.stop()
            self._capture_timer = None
        self._save_capture_wav()
        self._release_capture()
        self._is_capturing = False
        self.btn_capture.setText('Start Capture')

    def _release_capture(self) -> None:
        if self._capture_dev:
            self._capture_dev.close()
            self._capture_dev.release()
            self._capture_dev = None
        self._capture_cb = None

    def _update_capture_waveform(self) -> None:
        ch = self.spin_ch.value()
        bits = self._selected_bits()
        sr = self.spin_sr.value()
        display_sec = self.spin_display_sec.value()
        frame_bytes = ch * bits // 8
        max_bytes = display_sec * sr * frame_bytes

        total_bars = max(64, self.waveform_capture.width() // 2)
        buf = bytes(self._pcm_full_buffer)
        if len(buf) > max_bytes:
            buf = buf[-max_bytes:]

        # fixed bar density: pad leading zeros so bars fill right-to-left
        data_bars = max(1, int(len(buf) / max_bytes * total_bars)) if max_bytes > 0 else 0
        data_bars = min(data_bars, total_bars)
        empty_bars = total_bars - data_bars

        if data_bars > 0 and buf:
            peaks = pcm_to_peaks(buf, ch, bits, data_bars, absolute=True)
        else:
            peaks = []

        self.waveform_capture.set_peaks([0.0] * empty_bars + peaks)
        self.waveform_capture.set_playhead(1.0)

    def _save_capture_wav(self) -> None:
        if not self._pcm_full_buffer:
            print('No captured data to save')
            return
        name = self.edit_save_file.text().strip()
        if not name:
            name = 'cap.wav'
        if not os.path.isabs(name):
            name = str(Path.cwd() / name)
        ch = self.spin_ch.value()
        bits = self._selected_bits()
        sr = self.spin_sr.value()
        data = bytes(self._pcm_full_buffer)
        with wave.open(name, 'wb') as wf:
            wf.setnchannels(ch)
            wf.setsampwidth(bits // 8)
            wf.setframerate(sr)
            wf.writeframes(data)
        frame_bytes = ch * bits // 8
        duration = len(data) / (sr * frame_bytes)
        print(f'Saved: {name}, duration: {duration:.3f}s')

    # ---- Playback ----------------------------------------------------------

    def _browse_wav(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, 'Select WAV file', str(Path.cwd()), 'WAV Files (*.wav)')
        if path:
            self._wav_path = path
            self.lbl_file.setText(Path(path).name)
            self._load_wav_peaks(path)

    def _load_wav_peaks(self, path: str) -> None:
        try:
            with wave.open(path, 'rb') as wf:
                ch = wf.getnchannels()
                bits = wf.getsampwidth() * 8
                pcm = wf.readframes(wf.getnframes())
        except Exception as e:
            self.lbl_file.setText(f'Error: {e}')
            return
        target_bars = max(64, self.waveform_playback.width() // 2)
        peaks = pcm_to_peaks(pcm, ch, bits, target_bars, absolute=True)
        self.waveform_playback.set_peaks(peaks)

    def _toggle_playback(self) -> None:
        if self._is_playing:
            self._stop_playback()
        else:
            self._start_playback()

    def _start_playback(self) -> None:
        if not self._wav_path:
            self.lbl_file.setText('Please select a WAV file first')
            return

        self._playback_signals = PlaybackSignals()
        self._playback_signals.progress.connect(self._on_playback_progress)

        self._playback_dev = alsabridge.AlsaPlaybackDevice()
        self._playback_reader = alsabridge.WavReader(self._wav_path)
        self._playback_cb = alsabridge.WavPlaybackImpl(
            self._playback_reader, self._wav_path,
            on_progress=self._playback_signals.progress.emit)
        device_id = self.combo_playback.currentText() or 'default'
        self._playback_worker = _PlaybackRunner(
            self._playback_dev, self._playback_cb, device_id)
        self._playback_worker.playback_done.connect(self._on_playback_finished)
        self._playback_worker.error.connect(self._on_playback_error)
        self._playback_worker.start()

        self._is_playing = True
        self.btn_play.setText('Stop')
        self.btn_browse.setEnabled(False)
        self.waveform_playback.set_playhead(0.0)

    def _stop_playback(self) -> None:
        if self._playback_cb:
            self._playback_cb.should_stop_event.set()
        if self._playback_worker:
            if not self._playback_worker.wait(5000):
                if self._playback_dev:
                    self._playback_dev.stop()
                if not self._playback_worker.wait(1000):
                    return
        self._release_playback()
        self._is_playing = False
        self.btn_play.setText('Play')
        self.btn_browse.setEnabled(True)

    def _selected_bits(self) -> int:
        return int(self.combo_bits.currentData())

    def _release_playback(self) -> None:
        if self._playback_dev:
            self._playback_dev.close()
            self._playback_dev.release()
            self._playback_dev = None
        if self._playback_cb:
            self._playback_cb.close()
            self._playback_cb = None
        self._playback_reader = None
        self._playback_worker = None
        self._playback_signals = None

    def _on_seek_requested(self, ratio: float) -> None:
        if self._playback_cb and self._is_playing:
            self._playback_cb.seek_to_ratio(ratio)
            self.waveform_playback.set_playhead(ratio)

    def _on_playback_progress(self, ratio: float) -> None:
        self.waveform_playback.set_playhead(ratio)

    def _on_playback_finished(self) -> None:
        self._release_playback()
        self._is_playing = False
        self.btn_play.setText('Play')
        self.btn_browse.setEnabled(True)
        self.waveform_playback.set_playhead(1.0)

    def _on_playback_error(self, msg: str) -> None:
        self.lbl_file.setText(f'Error: {msg}')
        self._release_playback()
        self._is_playing = False
        self.btn_play.setText('Play')
        self.btn_browse.setEnabled(True)

    def closeEvent(self, event) -> None:
        if self._is_capturing:
            self._stop_capture()
        if self._is_playing:
            self._stop_playback()
        super().closeEvent(event)


if __name__ == '__main__':
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())
