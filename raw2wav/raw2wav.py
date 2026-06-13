# raw2wav.py
# Companion app for fpvGoggleAudioRecorder to convert raw 16-bit or 24-bit pcm files to wav files
# Compiling requires python 3, pillow, and PySide6
# Compile command (from raw2wav.py directory): pyinstaller --noconsole --onefile --add-data "background.png;." --add-data "icon.png;." raw2wav.py

import sys
import os
import wave
import threading
import subprocess

# Natively handle Windows taskbar ID registry to ensure the icon displays correctly
if sys.platform == "win32":
    import ctypes
    myappid = "mycompany.fpvconverter.raw2wav.1.0"  
    ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(myappid)

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QPushButton, QFileDialog, 
    QLabel, QVBoxLayout, QHBoxLayout, QTextBrowser, QTableWidget, QTableWidgetItem, QProgressBar,
    QGraphicsDropShadowEffect, QHeaderView, QComboBox
)
from PySide6.QtGui import QPixmap, QColor, QFont, QIcon
from PySide6.QtCore import Qt, Signal, QObject, QUrl

# -------------------------
# THREAD SAFE SIGNALS
# -------------------------

class Bridge(QObject):
    log = Signal(str)
    progress = Signal(int)
    max_progress = Signal(int)
    status = Signal(str)


def resource_path(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


def get_raw_file_size_str(filepath):
    """Calculates file size and returns a clean MB string layout."""
    try:
        bytes_size = os.path.getsize(filepath)
        mb_size = bytes_size / (1024 * 1024)
        return f"{mb_size:.2f} MB"
    except Exception:
        return "0.00 MB"


def get_raw_duration(filepath, bit_depth_index):
    """Calculates play time of raw PCM file based on sample width index (0=16bit, 1=24bit)."""
    try:
        file_size = os.path.getsize(filepath)
        # 16-bit = 2 bytes per sample. 24-bit = 3 bytes per sample.
        bytes_per_sample = 2 if bit_depth_index == 0 else 3
        bytes_per_second = 1 * bytes_per_sample * 44100
        
        total_seconds = int(file_size / bytes_per_second)
        minutes = total_seconds // 60
        seconds = total_seconds % 60
        return f"{minutes:02d}:{seconds:02d}"
    except Exception:
        return "00:00"


class MainWindow(QMainWindow):

    def __init__(self):
        super().__init__()

        self.setWindowTitle("FPV Converter PRO")
        self.setFixedSize(900, 750)
        self.setWindowIcon(QIcon(resource_path("icon.png")))

        self.files = []
        self.use_custom = False
        self.output_folder = ""

        # Initialize UI first so elements are ready for signals
        self.init_ui()

        self.bridge = Bridge()
        self.bridge.log.connect(self.log_msg)
        self.bridge.progress.connect(self.progress_set)
        self.bridge.max_progress.connect(self.progress.setMaximum)
        self.bridge.status.connect(self.status_set)

    # -------------------------
    # UI SETUP
    # -------------------------

    def init_ui(self):
        self.bg = QLabel(self)
        pix = QPixmap(resource_path("background.png"))
        self.bg.setPixmap(pix)
        self.bg.setScaledContents(True)
        self.bg.setGeometry(0, 0, 900, 750)
        self.bg.lower()

        container = QWidget(self)
        container.setGeometry(0, 0, 900, 750)
        container.setAttribute(Qt.WA_TranslucentBackground)

        layout = QVBoxLayout(container)
        layout.setContentsMargins(15, 15, 15, 15)
        layout.setSpacing(10)

        title = QLabel("FPV RAW → WAV CONVERTER PRO")
        title.setAlignment(Qt.AlignCenter)
        
        font = title.font()
        font.setPointSize(22)
        font.setBold(True)
        font.setLetterSpacing(QFont.AbsoluteSpacing, 1)
        title.setFont(font)
        
        palette = title.palette()
        palette.setColor(title.foregroundRole(), QColor("#FFFFFF"))
        title.setPalette(palette)
        title.setContentsMargins(10, 10, 10, 10)
        
        glow_effect = QGraphicsDropShadowEffect(title)
        glow_effect.setBlurRadius(20)
        glow_effect.setColor(QColor(0, 150, 255, 255))
        glow_effect.setOffset(0, 0)
        title.setGraphicsEffect(glow_effect)
        
        layout.addWidget(title)

        row = QHBoxLayout()
        self.btn_folder = QPushButton("Folder")
        self.btn_files = QPushButton("Files")
        self.btn_start = QPushButton("START")
        self.btn_start.setEnabled(False)

        self.default_style = """
        QPushButton {
            background: rgba(40,40,40,180);
            color: white;
            padding: 8px;
            border-radius: 6px;
        }
        QPushButton:hover {
            background: rgba(70,70,70,200);
        }
        QPushButton:disabled {
            background: rgba(25,25,25,140);
            color: rgba(255,255,255,120);
        }
        """

        self.start_style = """
        QPushButton {
            background: rgba(0,120,255,230);
            color: white;
            padding: 8px;
            border-radius: 6px;
            font-weight: bold;
        }
        QPushButton:hover {
            background: rgba(20,150,255,255);
        }
        """

        self.btn_folder.setStyleSheet(self.default_style)
        self.btn_files.setStyleSheet(self.default_style)
        self.btn_start.setStyleSheet(self.default_style)

        row.addWidget(self.btn_folder)
        row.addWidget(self.btn_files)
        row.addWidget(self.btn_start)
        layout.addLayout(row)

        body = QHBoxLayout()
        left_panel = QVBoxLayout()

        self.queue = QTableWidget()
        self.queue.setColumnCount(4)
        self.queue.setHorizontalHeaderLabels(["File Name", "Size", "Bit Depth", "Duration"])
        self.queue.verticalHeader().setVisible(False)
        self.queue.setSelectionBehavior(QTableWidget.SelectRows)
        self.queue.setEditTriggers(QTableWidget.NoEditTriggers)

        self.queue.setStyleSheet("""
        QTableWidget {
            background: rgba(0,0,0,140);
            color: white;
            gridline-color: rgba(255, 255, 255, 30);
            border-radius: 8px;
            font-family: Consolas;
            font-size: 12px;
        }
        QHeaderView::section {
            background-color: rgba(20, 20, 20, 180);
            color: #00FF88;
            font-family: Consolas;
            font-weight: bold;
            font-size: 11px;
            padding: 6px;
            border: none;
        }
        QTableWidget::item {
            padding: 4px;
        }
        """)

        header = self.queue.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.Stretch)
        header.setSectionResizeMode(1, QHeaderView.Interactive)
        header.setSectionResizeMode(2, QHeaderView.Interactive)
        header.setSectionResizeMode(3, QHeaderView.Interactive)
        self.queue.setColumnWidth(1, 90)
        self.queue.setColumnWidth(2, 100)
        self.queue.setColumnWidth(3, 85)

        left_panel.addWidget(self.queue)
        body.addLayout(left_panel, 2)

        right = QVBoxLayout()
        input_row = QHBoxLayout()
        self.input_dot = QLabel("●")
        self.input_label = QLabel("Input: None")
        input_row.addWidget(self.input_dot)
        input_row.addWidget(self.input_label)
        right.addLayout(input_row)

        self.custom_btn = QPushButton("Use Custom Output Folder")
        self.custom_btn.setCheckable(True)
        self.custom_btn.clicked.connect(self.toggle_output_mode)
        right.addWidget(self.custom_btn)

        output_row = QHBoxLayout()
        self.output_dot = QLabel("●")
        self.path_label = QLabel("Output: Default (same as input)")
        output_row.addWidget(self.output_dot)
        output_row.addWidget(self.path_label)
        right.addLayout(output_row)

        self.status = QLabel("Idle")
        self.status.setStyleSheet("color: white;")
        right.addWidget(self.status)

        self.progress = QProgressBar()
        self.progress.setFormat("%p% (%v/%m)")
        right.addWidget(self.progress)

        right.addStretch()
        body.addLayout(right, 1)
        layout.addLayout(body)

        self.log = QTextBrowser()
        self.log.setReadOnly(True)
        self.log.setOpenLinks(False)
        self.log.setStyleSheet("""
        QTextBrowser {
            background: rgba(0,0,0,160);
            color: #00FF88;
            font-family: Consolas;
        }
        a {
            color: #00FF88;
            text-decoration: underline;
        }
        a:hover {
            color: #00FFFF;
        }
        """)
        layout.addWidget(self.log)

        self.btn_folder.clicked.connect(self.select_folder)
        self.btn_files.clicked.connect(self.select_files)
        self.btn_start.clicked.connect(self.convert)
        self.log.anchorClicked.connect(self.open_path_in_explorer)
        self.queue.itemChanged.connect(self.handle_item_checked_changed)

        self.update_states()

    # -------------------------
    # UTILITIES / HANDLERS
    # -------------------------

    def open_path_in_explorer(self, url: QUrl):
        local_path = url.toLocalFile()
        if not os.path.exists(local_path):
            return

        try:
            if sys.platform == "win32":
                subprocess.run(['explorer', '/select,', os.path.normpath(local_path)])
            elif sys.platform == "darwin":
                subprocess.run(['open', '-R', local_path])
            else:
                folder = os.path.dirname(local_path)
                subprocess.run(['xdg-open', folder])
        except Exception as e:
            print(f"Failed to launch OS file browser: {e}")

    def update_states(self):
        checked_count = 0
        first_checked_file = ""
        
        for i in range(self.queue.rowCount()):
            item = self.queue.item(i, 0)
            if item and item.checkState() == Qt.Checked:
                checked_count += 1
                if not first_checked_file:
                    first_checked_file = self.files[i]

        valid = checked_count > 0
        active = "#00FF88"
        inactive = "red"
        color = active if valid else inactive

        label_style = f"color: {color}; font-size: 11px; font-family: Consolas; font-weight: bold;"
        self.input_dot.setStyleSheet(f"color: {color}; font-size: 14px;")
        self.output_dot.setStyleSheet(f"color: {color}; font-size: 14px;")
        self.input_label.setStyleSheet(label_style)
        self.path_label.setStyleSheet(label_style)

        if valid:
            if checked_count == 1:
                self.input_label.setText(f"Input: {os.path.basename(first_checked_file)}")
            else:
                self.input_label.setText(f"Input: {checked_count} files selected to convert")
        else:
            self.input_label.setText("Input: None (No files checked)")

        if self.use_custom and self.output_folder:
            self.path_label.setText(f"Output: {self.output_folder}")
        else:
            self.path_label.setText("Output: Default (same as input)")

        self.btn_start.setEnabled(valid)
        self.btn_start.setStyleSheet(self.start_style if valid else self.default_style)

    def handle_item_checked_changed(self, item):
        if item.column() == 0:
            self.update_states()

    def toggle_output_mode(self):
        if self.custom_btn.isChecked():
            folder = QFileDialog.getExistingDirectory(self, "Select Output Directory")
            if folder:
                self.use_custom = True
                self.output_folder = folder
            else:
                self.custom_btn.setChecked(False)
                self.use_custom = False
                self.output_folder = ""
        else:
            self.use_custom = False
            self.output_folder = ""
        
        self.update_states()

    def log_msg(self, msg):
        if "[INPUT]" in msg or "[OUTPUT]" in msg:
            prefix, path = msg.split("]", 1)
            path = path.strip()
            url = QUrl.fromLocalFile(path).toString()
            html_msg = f"{prefix}] <a href='{url}'>{path}</a>"
            self.log.append(html_msg)
        else:
            self.log.append(msg)

    def progress_set(self, v):
        self.progress.setValue(v)

    def status_set(self, s):
        self.status.setText(s)

    def select_folder(self):
        folder = QFileDialog.getExistingDirectory(self, "Select Folder Containing RAW Files")
        if not folder:
            return

        self.files = [
            os.path.join(folder, f)
            for f in os.listdir(folder)
            if f.lower().endswith(".raw")
        ]
        self.refresh_queue()
        self.update_states()

    def select_files(self):
        files, _ = QFileDialog.getOpenFileNames(
            self, "Select RAW files", "", "RAW Files (*.raw)"
        )
        if files:
            self.files = files
            self.refresh_queue()
            self.update_states()

    def update_row_duration(self, row, filepath, index):
        """Callback to update duration string dynamically when a user updates bitdepth selection."""
        duration_str = get_raw_duration(filepath, index)
        duration_item = QTableWidgetItem(duration_str)
        duration_item.setTextAlignment(Qt.AlignCenter)
        self.queue.setItem(row, 3, duration_item)

    def refresh_queue(self):
        self.queue.blockSignals(True)
        self.queue.setRowCount(0)
        self.queue.setRowCount(len(self.files))
        
        for row_index, f in enumerate(self.files):
            filename = os.path.basename(f)
            size_str = get_raw_file_size_str(f)
            
            name_item = QTableWidgetItem(filename)
            name_item.setFlags(name_item.flags() | Qt.ItemIsUserCheckable)
            name_item.setCheckState(Qt.Checked)
            self.queue.setItem(row_index, 0, name_item)
            
            size_item = QTableWidgetItem(size_str)
            size_item.setTextAlignment(Qt.AlignCenter) 
            self.queue.setItem(row_index, 1, size_item)
            
            # Interactive ComboBox inside Table Widget Row
            combo = QComboBox()
            combo.addItems(["16-bit", "24-bit"])
            
            # Smart Defaulting: Check if ".24bit" is part of the filename
            if ".24bit" in filename.lower():
                default_index = 1  # 24-bit
            else:
                default_index = 0  # 16-bit
                
            combo.setCurrentIndex(default_index)
            combo.setStyleSheet("""
                QComboBox {
                    background: rgba(30, 30, 30, 150);
                    color: #00FF88;
                    border: 1px solid rgba(255, 255, 255, 30);
                    border-radius: 4px;
                    padding-left: 5px;
                    font-family: Consolas;
                }
                QComboBox QAbstractItemView {
                    background: rgb(20, 20, 20);
                    color: white;
                    selection-background-color: rgba(0, 120, 255, 200);
                }
            """)
            # Explicitly tie dropdown updates to the duration layout engine
            combo.currentIndexChanged.connect(lambda idx, r=row_index, path=f: self.update_row_duration(r, path, idx))
            self.queue.setCellWidget(row_index, 2, combo)
            
            # Set initial duration string parsing using the detected file layout index
            duration_str = get_raw_duration(f, default_index)
            duration_item = QTableWidgetItem(duration_str)
            duration_item.setTextAlignment(Qt.AlignCenter)
            self.queue.setItem(row_index, 3, duration_item)
            
        self.queue.blockSignals(False)

    def convert(self):
        if not self.files:
            return

        # Safely scrape values on the main GUI thread before dispatching worker threads
        selected_targets = []
        for i in range(self.queue.rowCount()):
            item = self.queue.item(i, 0)
            if item and item.checkState() == Qt.Checked:
                combo = self.queue.cellWidget(i, 2)
                bit_depth = 16 if combo.currentIndex() == 0 else 24
                selected_targets.append((self.files[i], bit_depth))

        if not selected_targets:
            return

        use_custom = self.use_custom
        output_folder = self.output_folder

        def worker(targets, custom_mode, custom_path):
            total = len(targets)
            self.bridge.max_progress.emit(total)
            self.bridge.progress.emit(0)
            self.bridge.log.emit("---- CONVERSION START ----")

            for progress_index, (f, bit_depth) in enumerate(targets):
                try:
                    out = custom_path if custom_mode else os.path.dirname(f)
                    wav = os.path.join(
                        out, os.path.splitext(os.path.basename(f))[0] + ".wav"
                    )

                    self.bridge.log.emit(f"[INPUT]  {os.path.abspath(f)} ({bit_depth}-bit)")

                    with open(f, "rb") as r:
                        raw_bytes = r.read()

                    processed_frames = bytearray()
                    sampwidth = 2
                    
                    if bit_depth == 16:
                        sampwidth = 2
                        processed_frames = raw_bytes
                        
                    elif bit_depth == 24:
                        sampwidth = 3
                        remainder = len(raw_bytes) % 3
                        if remainder != 0:
                            raw_bytes = raw_bytes[:-remainder]

                        num_samples = len(raw_bytes) // 3
                        for i in range(num_samples):
                            chunk = raw_bytes[i*3 : (i+1)*3]
                            val = int.from_bytes(chunk, byteorder='little', signed=True)
                            processed_frames.extend(val.to_bytes(3, byteorder='little', signed=True))

                    with wave.open(wav, "wb") as w:
                        w.setnchannels(1)
                        w.setsampwidth(sampwidth)
                        w.setframerate(44100)
                        w.writeframes(processed_frames)

                    self.bridge.log.emit(f"[OUTPUT] {os.path.abspath(wav)}")

                except Exception as e:
                    self.bridge.log.emit(f"[ERROR] {e}")

                self.bridge.progress.emit(progress_index + 1)
                self.bridge.status.emit(f"{progress_index + 1}/{total}")

            self.bridge.log.emit("---- CONVERSION COMPLETE ----")
            self.bridge.status.emit("Done")

        threading.Thread(
            target=worker, 
            args=(selected_targets, use_custom, output_folder), 
            daemon=True
        ).start()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())