#!/usr/bin/env python3
"""
ESP32 Hand Controller Configuration Interface - Qt Version
A modern GUI application to configure hand controller settings via USB serial
"""

import sys
import os
import subprocess
import importlib.util
import platform
import shutil
import json
import time
import threading
import queue
import tempfile
import shutil
import glob
import re
from datetime import datetime
from packaging import version

import serial
import requests
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, 
    QGridLayout, QLabel, QPushButton, QLineEdit, QComboBox, 
    QCheckBox, QTextEdit, QGroupBox, QFrame, QSplitter,
    QMessageBox, QFileDialog, QProgressBar, QTabWidget,
    QScrollArea, QSizePolicy, QSpacerItem, QButtonGroup,
    QRadioButton, QSpinBox, QDoubleSpinBox, QSlider
)
from PyQt6.QtCore import (
    Qt, QThread, pyqtSignal, QTimer, QSize, QPropertyAnimation,
    QEasingCurve, QRect, QPoint
)
from PyQt6.QtGui import (
    QFont, QPalette, QColor, QPixmap, QIcon, QPainter,
    QLinearGradient, QBrush, QPen
)

class SerialWorker(QThread):
    """Worker thread for serial communication"""
    data_received = pyqtSignal(str)
    connection_changed = pyqtSignal(bool)
    
    def __init__(self):
        super().__init__()
        self.serial_port = None
        self.is_connected = False
        self.running = True
        
    def connect(self, port, baudrate=115200):
        """Connect to serial port"""
        try:
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baudrate,
                timeout=1,
                write_timeout=1
            )
            self.is_connected = True
            self.connection_changed.emit(True)
            return True
        except Exception as e:
            self.connection_changed.emit(False)
            return False
    
    def disconnect(self):
        """Disconnect from serial port"""
        self.is_connected = False
        if self.serial_port:
            try:
                self.serial_port.close()
            except:
                pass
            self.serial_port = None
        self.connection_changed.emit(False)
    
    def send_command(self, command):
        """Send command via serial"""
        if self.is_connected and self.serial_port:
            try:
                self.serial_port.write(f"{command}\n".encode())
                return True
            except:
                return False
        return False
    
    def run(self):
        """Main thread loop"""
        while self.running:
            if self.is_connected and self.serial_port:
                try:
                    if self.serial_port.in_waiting:
                        line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            self.data_received.emit(line)
                except:
                    self.is_connected = False
                    self.connection_changed.emit(False)
            time.sleep(0.01)
    
    def stop(self):
        """Stop the worker thread"""
        self.running = False
        self.disconnect()

class ModernButton(QPushButton):
    """Custom modern-styled button"""
    def __init__(self, text, primary=False, *args, **kwargs):
        super().__init__(text, *args, **kwargs)
        self.primary = primary
        self.setup_style()
        
    def setup_style(self):
        """Setup modern button styling"""
        if self.primary:
            self.setStyleSheet("""
                QPushButton {
                    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                        stop:0 #4A90E2, stop:1 #357ABD);
                    border: none;
                    border-radius: 8px;
                    color: white;
                    font-weight: bold;
                    padding: 10px 20px;
                    font-size: 14px;
                }
                QPushButton:hover {
                    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                        stop:0 #5BA0F2, stop:1 #4A90E2);
                }
                QPushButton:pressed {
                    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                        stop:0 #357ABD, stop:1 #2E6BA8);
                }
                QPushButton:disabled {
                    background: #CCCCCC;
                    color: #666666;
                }
            """)
        else:
            self.setStyleSheet("""
                QPushButton {
                    background: white;
                    border: 2px solid #E0E0E0;
                    border-radius: 8px;
                    color: #333333;
                    font-weight: 500;
                    padding: 8px 16px;
                    font-size: 14px;
                }
                QPushButton:hover {
                    border-color: #4A90E2;
                    color: #4A90E2;
                }
                QPushButton:pressed {
                    background: #F0F8FF;
                    border-color: #357ABD;
                }
                QPushButton:disabled {
                    background: #F5F5F5;
                    color: #CCCCCC;
                    border-color: #E0E0E0;
                }
            """)

class ModernGroupBox(QGroupBox):
    """Custom modern-styled group box"""
    def __init__(self, title="", *args, **kwargs):
        super().__init__(title, *args, **kwargs)
        self.setup_style()
        
    def setup_style(self):
        """Setup modern group box styling"""
        self.setStyleSheet("""
            QGroupBox {
                font-weight: bold;
                font-size: 16px;
                color: #333333;
                border: 2px solid #E0E0E0;
                border-radius: 12px;
                margin-top: 10px;
                padding-top: 10px;
                background: white;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 15px;
                padding: 0 10px 0 10px;
                background: white;
            }
        """)

class ToolTipWidget(QWidget):
    """Custom tooltip widget"""
    def __init__(self, text, parent=None):
        super().__init__(parent)
        self.setWindowFlags(Qt.WindowType.ToolTip | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setup_ui(text)
        
    def setup_ui(self, text):
        layout = QVBoxLayout()
        layout.setContentsMargins(10, 8, 10, 8)
        
        label = QLabel(text)
        label.setWordWrap(True)
        label.setMaximumWidth(300)
        label.setStyleSheet("""
            QLabel {
                background: rgba(255, 255, 224, 0.95);
                border: 1px solid #D0D0D0;
                border-radius: 6px;
                padding: 8px;
                color: #333333;
                font-size: 12px;
            }
        """)
        
        layout.addWidget(label)
        self.setLayout(layout)

class ESP32ControllerQt(QMainWindow):
    """Main application window"""
    
    # Define signals
    flash_log_signal = pyqtSignal(str)
    flash_complete_signal = pyqtSignal(bool, str)
    
    def __init__(self):
        super().__init__()
        self.serial_worker = SerialWorker()
        self.config = {
            'invert_throttle': False,
            'level_assistant': False,
            'speed_unit_mph': False,
            'motor_pulley': 15,
            'wheel_pulley': 33,
            'wheel_diameter_mm': 115,
            'motor_poles': 14,
            'ble_connected': False,
            'firmware_version': 'Unknown',
            'pid_kp': 0.8,
            'pid_ki': 0.5,
            'pid_kd': 0.05,
            'pid_output_max': 48.0
        }
        
        self.setup_ui()
        self.setup_connections()
        self.setup_serial_worker()
        
    def setup_ui(self):
        """Setup the user interface"""
        self.setWindowTitle("ESP32 Hand Controller Configuration - Qt")
        self.setMinimumSize(1200, 1100)
        self.resize(1500, 1100)
        
        # Set application style
        self.setStyleSheet("""
            QMainWindow {
                background: #F8F9FA;
            }
            QLabel {
                color: #333333;
                font-size: 14px;
            }
            QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
                border: 2px solid #E0E0E0;
                border-radius: 6px;
                padding: 8px 12px;
                font-size: 14px;
                background: white;
            }
            QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
                border-color: #4A90E2;
            }
            QCheckBox {
                font-size: 14px;
                color: #333333;
            }
            QCheckBox::indicator {
                width: 20px;
                height: 20px;
            }
            QCheckBox::indicator:unchecked {
                border: 2px solid #E0E0E0;
                border-radius: 4px;
                background: white;
            }
            QCheckBox::indicator:checked {
                border: 2px solid #4A90E2;
                border-radius: 4px;
                background: #4A90E2;
                image: url(data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMTIiIGhlaWdodD0iOSIgdmlld0JveD0iMCAwIDEyIDkiIGZpbGw9Im5vbmUiIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CjxwYXRoIGQ9Ik0xIDQuNUw0LjUgOEwxMSAxIiBzdHJva2U9IndoaXRlIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCIvPgo8L3N2Zz4K);
            }
            QTextEdit {
                border: 2px solid #E0E0E0;
                border-radius: 8px;
                background: white;
                font-family: 'Consolas', 'Monaco', monospace;
                font-size: 12px;
                padding: 8px;
            }
            QTextEdit:focus {
                border-color: #4A90E2;
            }
        """)
        
        # Create central widget
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        
        # Create main layout
        main_layout = QHBoxLayout(central_widget)
        main_layout.setContentsMargins(20, 20, 20, 20)
        main_layout.setSpacing(20)
        
        # Create splitter for resizable panels
        splitter = QSplitter(Qt.Orientation.Horizontal)
        main_layout.addWidget(splitter)
        
        # Left panel - Configuration
        self.create_config_panel(splitter)
        
        # Right panel - Response and Status
        self.create_response_panel(splitter)
        
        # Set splitter proportions
        splitter.setSizes([800, 600])
        
    def create_config_panel(self, parent):
        """Create the configuration panel"""
        config_widget = QWidget()
        config_widget.setMaximumWidth(800)
        layout = QVBoxLayout(config_widget)
        layout.setSpacing(15)
        
        # Connection section
        self.create_connection_section(layout)
        
        # Configuration sections
        self.create_basic_config_section(layout)
        self.create_pid_config_section(layout)
        self.create_firmware_section(layout)
        self.create_actions_section(layout)
        
        # Add stretch to push everything to top
        layout.addStretch()
        
        parent.addWidget(config_widget)
        
    def create_connection_section(self, layout):
        """Create connection section"""
        group = ModernGroupBox("Connection")
        group_layout = QGridLayout(group)
        
        # Port selection
        group_layout.addWidget(QLabel("Port:"), 0, 0)
        self.port_combo = QComboBox()
        self.port_combo.addItems(self.get_available_ports())
        self.port_combo.setCurrentText("/dev/ttyACM0")
        group_layout.addWidget(self.port_combo, 0, 1)
        
        # Connect button
        self.connect_btn = ModernButton("Connect", primary=True)
        self.connect_btn.clicked.connect(self.toggle_connection)
        group_layout.addWidget(self.connect_btn, 0, 2)
        
        # Status label
        self.status_label = QLabel("Disconnected")
        self.status_label.setStyleSheet("color: #E74C3C; font-weight: bold;")
        group_layout.addWidget(self.status_label, 0, 3)
        
        # Refresh button
        refresh_btn = ModernButton("Refresh")
        refresh_btn.clicked.connect(self.refresh_ports)
        group_layout.addWidget(refresh_btn, 0, 4)
        
        layout.addWidget(group)
        
    def create_basic_config_section(self, layout):
        """Create basic configuration section"""
        group = ModernGroupBox("Basic Configuration")
        group_layout = QVBoxLayout(group)
        
        # Throttle inversion
        throttle_layout = QHBoxLayout()
        throttle_label = QLabel("Throttle Inversion:")
        throttle_label.setMinimumWidth(150)
        throttle_layout.addWidget(throttle_label)
        
        # Help button for throttle
        help_btn = QPushButton("?")
        help_btn.setFixedSize(25, 25)
        help_btn.setStyleSheet("""
            QPushButton {
                background: #4A90E2;
                color: white;
                border: none;
                border-radius: 12px;
                font-weight: bold;
            }
            QPushButton:hover {
                background: #357ABD;
            }
        """)
        help_btn.clicked.connect(self.show_throttle_help)
        throttle_layout.addWidget(help_btn)
        
        throttle_layout.addStretch()
        
        self.throttle_check = QCheckBox("Inverted (like Boosted boards)")
        self.throttle_check.toggled.connect(self.toggle_throttle)
        throttle_layout.addWidget(self.throttle_check)
        
        group_layout.addLayout(throttle_layout)
        
        # Level assistant
        level_layout = QHBoxLayout()
        level_label = QLabel("Level Assistant:")
        level_label.setMinimumWidth(150)
        level_layout.addWidget(level_label)
        
        # Help button for level assistant
        level_help_btn = QPushButton("?")
        level_help_btn.setFixedSize(25, 25)
        level_help_btn.setStyleSheet(help_btn.styleSheet())
        level_help_btn.clicked.connect(self.show_level_assistant_help)
        level_layout.addWidget(level_help_btn)
        
        level_layout.addStretch()
        
        self.level_assist_check = QCheckBox("Enabled")
        self.level_assist_check.toggled.connect(self.toggle_level_assistant)
        level_layout.addWidget(self.level_assist_check)
        
        group_layout.addLayout(level_layout)
        
        # Speed unit
        speed_layout = QHBoxLayout()
        speed_label = QLabel("Speed Unit:")
        speed_label.setMinimumWidth(150)
        speed_layout.addWidget(speed_label)
        
        # Help button for speed unit
        speed_help_btn = QPushButton("?")
        speed_help_btn.setFixedSize(25, 25)
        speed_help_btn.setStyleSheet(help_btn.styleSheet())
        speed_help_btn.clicked.connect(self.show_speed_unit_help)
        speed_layout.addWidget(speed_help_btn)
        
        speed_layout.addStretch()
        
        self.speed_unit_check = QCheckBox("mi/h (unchecked = km/h)")
        self.speed_unit_check.toggled.connect(self.toggle_speed_unit)
        speed_layout.addWidget(self.speed_unit_check)
        
        group_layout.addLayout(speed_layout)
        
        # Motor and wheel configuration
        config_grid = QGridLayout()
        
        # Motor pulley
        config_grid.addWidget(QLabel("Motor Pulley Teeth:"), 0, 0)
        self.pulley_spin = QSpinBox()
        self.pulley_spin.setRange(1, 255)
        self.pulley_spin.setValue(15)
        config_grid.addWidget(self.pulley_spin, 0, 1)
        pulley_btn = ModernButton("Set")
        pulley_btn.clicked.connect(self.set_motor_pulley)
        config_grid.addWidget(pulley_btn, 0, 2)
        
        # Wheel pulley
        config_grid.addWidget(QLabel("Wheel Pulley Teeth:"), 1, 0)
        self.wheel_pulley_spin = QSpinBox()
        self.wheel_pulley_spin.setRange(1, 255)
        self.wheel_pulley_spin.setValue(33)
        config_grid.addWidget(self.wheel_pulley_spin, 1, 1)
        wheel_pulley_btn = ModernButton("Set")
        wheel_pulley_btn.clicked.connect(self.set_wheel_pulley)
        config_grid.addWidget(wheel_pulley_btn, 1, 2)
        
        # Wheel diameter
        config_grid.addWidget(QLabel("Wheel Diameter (mm):"), 2, 0)
        self.wheel_spin = QSpinBox()
        self.wheel_spin.setRange(1, 255)
        self.wheel_spin.setValue(115)
        config_grid.addWidget(self.wheel_spin, 2, 1)
        wheel_btn = ModernButton("Set")
        wheel_btn.clicked.connect(self.set_wheel_size)
        config_grid.addWidget(wheel_btn, 2, 2)
        
        # Motor poles
        config_grid.addWidget(QLabel("Motor Poles:"), 3, 0)
        self.poles_spin = QSpinBox()
        self.poles_spin.setRange(1, 255)
        self.poles_spin.setValue(14)
        config_grid.addWidget(self.poles_spin, 3, 1)
        poles_btn = ModernButton("Set")
        poles_btn.clicked.connect(self.set_motor_poles)
        config_grid.addWidget(poles_btn, 3, 2)
        
        group_layout.addLayout(config_grid)
        layout.addWidget(group)
        
    def create_pid_config_section(self, layout):
        """Create PID configuration section"""
        group = ModernGroupBox("Level Assistant PID Tuning")
        group_layout = QVBoxLayout(group)
        
        # PID parameters grid
        pid_grid = QGridLayout()
        
        # Kp
        pid_grid.addWidget(QLabel("Kp (Proportional):"), 0, 0)
        self.pid_kp_spin = QDoubleSpinBox()
        self.pid_kp_spin.setRange(0.0, 10.0)
        self.pid_kp_spin.setSingleStep(0.1)
        self.pid_kp_spin.setValue(0.8)
        self.pid_kp_spin.setDecimals(2)
        pid_grid.addWidget(self.pid_kp_spin, 0, 1)
        kp_btn = ModernButton("Set")
        kp_btn.clicked.connect(self.set_pid_kp)
        pid_grid.addWidget(kp_btn, 0, 2)
        
        # Ki
        pid_grid.addWidget(QLabel("Ki (Integral):"), 1, 0)
        self.pid_ki_spin = QDoubleSpinBox()
        self.pid_ki_spin.setRange(0.0, 2.0)
        self.pid_ki_spin.setSingleStep(0.01)
        self.pid_ki_spin.setValue(0.5)
        self.pid_ki_spin.setDecimals(3)
        pid_grid.addWidget(self.pid_ki_spin, 1, 1)
        ki_btn = ModernButton("Set")
        ki_btn.clicked.connect(self.set_pid_ki)
        pid_grid.addWidget(ki_btn, 1, 2)
        
        # Kd
        pid_grid.addWidget(QLabel("Kd (Derivative):"), 2, 0)
        self.pid_kd_spin = QDoubleSpinBox()
        self.pid_kd_spin.setRange(0.0, 1.0)
        self.pid_kd_spin.setSingleStep(0.01)
        self.pid_kd_spin.setValue(0.05)
        self.pid_kd_spin.setDecimals(3)
        pid_grid.addWidget(self.pid_kd_spin, 2, 1)
        kd_btn = ModernButton("Set")
        kd_btn.clicked.connect(self.set_pid_kd)
        pid_grid.addWidget(kd_btn, 2, 2)
        
        # Output Max
        pid_grid.addWidget(QLabel("Output Max:"), 3, 0)
        self.pid_output_max_spin = QDoubleSpinBox()
        self.pid_output_max_spin.setRange(10.0, 100.0)
        self.pid_output_max_spin.setSingleStep(1.0)
        self.pid_output_max_spin.setValue(48.0)
        self.pid_output_max_spin.setDecimals(1)
        pid_grid.addWidget(self.pid_output_max_spin, 3, 1)
        output_max_btn = ModernButton("Set")
        output_max_btn.clicked.connect(self.set_pid_output_max)
        pid_grid.addWidget(output_max_btn, 3, 2)
        
        group_layout.addLayout(pid_grid)
        
        # PID action buttons
        pid_actions = QHBoxLayout()
        get_pid_btn = ModernButton("Get PID Parameters")
        get_pid_btn.clicked.connect(self.get_pid_params)
        pid_actions.addWidget(get_pid_btn)
        
        load_defaults_btn = ModernButton("Load Defaults")
        load_defaults_btn.clicked.connect(self.load_pid_defaults)
        pid_actions.addWidget(load_defaults_btn)
        
        group_layout.addLayout(pid_actions)
        layout.addWidget(group)
        
    def create_firmware_section(self, layout):
        """Create firmware section"""
        group = ModernGroupBox("Firmware Management")
        group_layout = QVBoxLayout(group)
        
        # ESP-IDF Path
        idf_layout = QHBoxLayout()
        idf_layout.addWidget(QLabel("ESP-IDF Path:"))
        self.idf_path_edit = QLineEdit()
        self.idf_path_edit.setText(self.detect_esp_idf_path())
        idf_layout.addWidget(self.idf_path_edit)
        browse_idf_btn = ModernButton("Browse")
        browse_idf_btn.clicked.connect(self.browse_idf_path)
        idf_layout.addWidget(browse_idf_btn)
        
        group_layout.addLayout(idf_layout)
        
        # Firmware file
        firmware_layout = QHBoxLayout()
        firmware_layout.addWidget(QLabel("Firmware File:"))
        self.firmware_path_edit = QLineEdit()
        firmware_layout.addWidget(self.firmware_path_edit)
        browse_firmware_btn = ModernButton("Browse")
        browse_firmware_btn.clicked.connect(self.browse_firmware_file)
        firmware_layout.addWidget(browse_firmware_btn)
        
        group_layout.addLayout(firmware_layout)
        
        # Firmware action buttons
        firmware_actions = QHBoxLayout()
        download_btn = ModernButton("Download Latest")
        download_btn.clicked.connect(self.download_latest_firmware)
        firmware_actions.addWidget(download_btn)
        
        flash_btn = ModernButton("Flash Firmware", primary=True)
        flash_btn.clicked.connect(self.flash_firmware)
        firmware_actions.addWidget(flash_btn)
        
        check_update_btn = ModernButton("Check Updates")
        check_update_btn.clicked.connect(self.check_firmware_update)
        firmware_actions.addWidget(check_update_btn)
        
        group_layout.addLayout(firmware_actions)
        layout.addWidget(group)
        
    def create_actions_section(self, layout):
        """Create actions section"""
        group = ModernGroupBox("Actions")
        group_layout = QVBoxLayout(group)
        
        # Action buttons grid
        actions_grid = QGridLayout()
        
        buttons = [
            ("Reset Odometer", self.reset_odometer),
            ("Get Config", self.get_config),
            ("Calibrate Throttle", self.calibrate_throttle),
            ("Get Calibration", self.get_calibration),
            ("Help", self.show_help),
        ]
        
        for i, (text, command) in enumerate(buttons):
            row = i // 3
            col = i % 3
            btn = ModernButton(text)
            btn.clicked.connect(command)
            actions_grid.addWidget(btn, row, col)
        
        group_layout.addLayout(actions_grid)
        layout.addWidget(group)
        
    def create_response_panel(self, parent):
        """Create the response panel"""
        response_widget = QWidget()
        layout = QVBoxLayout(response_widget)
        layout.setSpacing(15)
        
        # Response text area
        response_group = ModernGroupBox("Response")
        response_layout = QVBoxLayout(response_group)
        
        self.response_text = QTextEdit()
        self.response_text.setReadOnly(True)
        self.response_text.setMinimumHeight(400)
        response_layout.addWidget(self.response_text)
        
        # Clear button
        clear_btn = ModernButton("Clear")
        clear_btn.clicked.connect(self.clear_response)
        response_layout.addWidget(clear_btn)
        
        layout.addWidget(response_group)
        
        # Current configuration display
        config_group = ModernGroupBox("Current Configuration")
        config_layout = QVBoxLayout(config_group)
        
        self.config_display = QTextEdit()
        self.config_display.setReadOnly(True)
        self.config_display.setMaximumHeight(200)
        self.config_display.setStyleSheet("""
            QTextEdit {
                background: #F8F9FA;
                border: 1px solid #E0E0E0;
                font-family: 'Consolas', 'Monaco', monospace;
                font-size: 11px;
            }
        """)
        config_layout.addWidget(self.config_display)
        
        layout.addWidget(config_group)
        parent.addWidget(response_widget)
        
    def setup_connections(self):
        """Setup signal connections"""
        # Serial worker connections
        self.serial_worker.data_received.connect(self.process_response)
        self.serial_worker.connection_changed.connect(self.on_connection_changed)
        
        # Flash signals
        self.flash_log_signal.connect(self.log_message)
        self.flash_complete_signal.connect(self.on_flash_complete)
        
    def setup_serial_worker(self):
        """Setup and start serial worker"""
        self.serial_worker.start()
        
    def get_available_ports(self):
        """Get list of available serial ports"""
        if sys.platform.startswith('win'):
            ports = ['COM%s' % (i + 1) for i in range(256)]
        elif sys.platform.startswith('linux') or sys.platform.startswith('cygwin'):
            ports = glob.glob('/dev/tty[A-Za-z]*')
        elif sys.platform.startswith('darwin'):
            ports = glob.glob('/dev/tty.*')
        else:
            return []
        
        result = []
        for port in ports:
            try:
                s = serial.Serial(port)
                s.close()
                result.append(port)
            except (OSError, serial.SerialException):
                pass
        return result
        
    def refresh_ports(self):
        """Refresh available ports"""
        self.port_combo.clear()
        self.port_combo.addItems(self.get_available_ports())
        
    def toggle_connection(self):
        """Toggle serial connection"""
        if not self.serial_worker.is_connected:
            self.connect()
        else:
            self.disconnect()
            
    def connect(self):
        """Connect to ESP32"""
        port = self.port_combo.currentText()
        if self.serial_worker.connect(port):
            self.log_message("Connected to ESP32 Hand Controller")
        else:
            QMessageBox.critical(self, "Connection Error", "Failed to connect to ESP32")
            
    def disconnect(self):
        """Disconnect from ESP32"""
        self.serial_worker.disconnect()
        self.log_message("Disconnected from ESP32")
        
    def on_connection_changed(self, connected):
        """Handle connection state change"""
        if connected:
            self.connect_btn.setText("Disconnect")
            self.status_label.setText("Connected")
            self.status_label.setStyleSheet("color: #27AE60; font-weight: bold;")
        else:
            self.connect_btn.setText("Connect")
            self.status_label.setText("Disconnected")
            self.status_label.setStyleSheet("color: #E74C3C; font-weight: bold;")
            
    def send_serial_command(self, command):
        """Send command via serial"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return False
            
        if self.serial_worker.send_command(command):
            self.log_message(f"Sent: {command}")
            return True
        else:
            QMessageBox.critical(self, "Send Error", "Failed to send command")
            return False
            
    def process_response(self, response):
        """Process response from ESP32"""
        cleaned_response = self.clean_response(response)
        if cleaned_response:
            self.log_message(cleaned_response)
            self.parse_config_response(cleaned_response)
            
    def clean_response(self, response):
        """Clean up response by removing ANSI codes and verbose logging"""
        # Remove ANSI color codes
        ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
        cleaned = ansi_escape.sub('', response)
        
        # Remove verbose logging lines
        lines = cleaned.split('\n')
        filtered_lines = []
        
        for line in lines:
            line = line.strip()
            if not line or line in ['>', '']:
                continue
                
            # Skip verbose logging lines
            if any(skip_pattern in line for skip_pattern in [
                'I (', 'USB_SERIAL: Processing command:',
                'USB_SERIAL: Parsed command type:',
                'USB_SERIAL: Motor pulley teeth set to:',
                'USB_SERIAL: Wheel pulley teeth set to:',
                'USB_SERIAL: Wheel diameter set to:',
                'USB_SERIAL: Motor poles set to:',
                'USB_SERIAL: Throttle inversion:',
                'USB_SERIAL: Level assistant:',
                'USB_SERIAL: Odometer reset',
                'USB_SERIAL: Configuration:',
                'USB_SERIAL: Available commands:',
                'USB_SERIAL: Unknown command:'
            ]):
                continue
                
            filtered_lines.append(line)
            
        return '\n'.join(filtered_lines) if filtered_lines else None
        
    def parse_config_response(self, response):
        """Parse configuration response"""
        try:
            # Parse throttle inversion
            if "Throttle inversion: ENABLED" in response:
                self.config['invert_throttle'] = True
                self.throttle_check.setChecked(True)
            elif "Throttle inversion: DISABLED" in response:
                self.config['invert_throttle'] = False
                self.throttle_check.setChecked(False)
                
            # Parse level assistant
            if "Level assistant: ENABLED" in response:
                self.config['level_assistant'] = True
                self.level_assist_check.setChecked(True)
            elif "Level assistant: DISABLED" in response:
                self.config['level_assistant'] = False
                self.level_assist_check.setChecked(False)
                
            # Parse speed unit
            if "Speed Unit:" in response:
                self.config['speed_unit_mph'] = "mi/h" in response
                self.speed_unit_check.setChecked(self.config['speed_unit_mph'])
                
            # Parse PID parameters
            if "PID Kp set to:" in response:
                try:
                    kp = float(response.split("PID Kp set to:")[1].strip())
                    self.config['pid_kp'] = kp
                    self.pid_kp_spin.setValue(kp)
                except:
                    pass
                    
            if "PID Ki set to:" in response:
                try:
                    ki = float(response.split("PID Ki set to:")[1].strip())
                    self.config['pid_ki'] = ki
                    self.pid_ki_spin.setValue(ki)
                except:
                    pass
                    
            if "PID Kd set to:" in response:
                try:
                    kd = float(response.split("PID Kd set to:")[1].strip())
                    self.config['pid_kd'] = kd
                    self.pid_kd_spin.setValue(kd)
                except:
                    pass
                    
            if "PID Output Max set to:" in response:
                try:
                    output_max = float(response.split("PID Output Max set to:")[1].strip())
                    self.config['pid_output_max'] = output_max
                    self.pid_output_max_spin.setValue(output_max)
                except:
                    pass
                    
            # Parse motor/wheel settings
            if "Motor pulley teeth set to:" in response:
                try:
                    teeth = int(response.split("Motor pulley teeth set to:")[1].strip())
                    self.config['motor_pulley'] = teeth
                    self.pulley_spin.setValue(teeth)
                except:
                    pass
                    
            if "Wheel pulley teeth set to:" in response:
                try:
                    teeth = int(response.split("Wheel pulley teeth set to:")[1].strip())
                    self.config['wheel_pulley'] = teeth
                    self.wheel_pulley_spin.setValue(teeth)
                except:
                    pass
                    
            if "Wheel diameter set to:" in response:
                try:
                    size = int(response.split("Wheel diameter set to:")[1].split("mm")[0].strip())
                    self.config['wheel_diameter_mm'] = size
                    self.wheel_spin.setValue(size)
                except:
                    pass
                    
            if "Motor poles set to:" in response:
                try:
                    poles = int(response.split("Motor poles set to:")[1].strip())
                    self.config['motor_poles'] = poles
                    self.poles_spin.setValue(poles)
                except:
                    pass
                    
            # Parse firmware version
            if "Firmware version:" in response or "Firmware Version:" in response:
                try:
                    version_text = response.split(":")[1].strip()
                    self.config['firmware_version'] = version_text
                except:
                    pass
                    
            # Update configuration display
            self.update_config_display()
            
        except Exception as e:
            print(f"Error parsing config response: {e}")
            
    def update_config_display(self):
        """Update the configuration display"""
        config_text = f"""Firmware Version: {self.config['firmware_version']}
Throttle Inverted: {'Yes' if self.config['invert_throttle'] else 'No'}
Level Assistant: {'Yes' if self.config['level_assistant'] else 'No'}
Speed Unit: {'mi/h' if self.config['speed_unit_mph'] else 'km/h'}
Motor Pulley Teeth: {self.config['motor_pulley']}
Wheel Pulley Teeth: {self.config['wheel_pulley']}
Wheel Diameter: {self.config['wheel_diameter_mm']} mm
Motor Poles: {self.config['motor_poles']}
BLE Connected: {'Yes' if self.config['ble_connected'] else 'No'}
PID Kp: {self.config['pid_kp']}
PID Ki: {self.config['pid_ki']}
PID Kd: {self.config['pid_kd']}
PID Output Max: {self.config['pid_output_max']}"""
        
        self.config_display.setPlainText(config_text)
        
    def log_message(self, message):
        """Add message to response text area"""
        # Format different types of messages
        if message.startswith("Sent:"):
            self.response_text.append(f"  {message}")
        elif "set to:" in message:
            self.response_text.append(f"[OK] {message}")
        elif "Throttle inversion:" in message:
            status = "enabled" if "ENABLED" in message else "disabled"
            self.response_text.append(f"[OK] Throttle inversion: {status}")
        elif "Level assistant:" in message:
            status = "enabled" if "ENABLED" in message else "disabled"
            self.response_text.append(f"[OK] Level assistant: {status}")
        elif "Odometer reset" in message:
            self.response_text.append(f"[OK] {message}")
        elif "Unknown command:" in message:
            self.response_text.append(f"[ERROR] {message}")
        elif "=== Hand Controller Commands ===" in message:
            self.response_text.append(message)
        elif "help" in message and "Show this help message" in message:
            pass
        elif any(cmd in message for cmd in ["invert_throttle", "level_assistant", "reset_odometer", "set_motor_pulley",
                                          "set_wheel_pulley", "set_wheel_size", "set_motor_poles", "get_config"]):
            self.response_text.append(f"  {message}")
        elif "calibrate_throttle" in message and "Manually calibrate throttle range" in message:
            self.response_text.append(f"  {message}")
        elif "get_calibration" in message and "Show throttle calibration status" in message:
            self.response_text.append(f"  {message}")
        elif "Calibration progress:" in message:
            self.response_text.append(f"[PROGRESS] {message}")
        elif "Calibration complete!" in message:
            self.response_text.append(f"[OK] {message}")
        elif "Calibration failed" in message:
            self.response_text.append(f"[ERROR] {message}")
        elif "Raw range:" in message or "Calibrated range:" in message:
            self.response_text.append(f"  {message}")
        elif "Calibration Status:" in message:
            self.response_text.append(f"[STATUS] {message}")
        elif "Current ADC Reading:" in message or "Current Mapped Value:" in message:
            self.response_text.append(f"  {message}")
        elif "Throttle signals were set to neutral during calibration" in message:
            self.response_text.append(f"[SAFETY] {message}")
        elif "Calibrated Min Value:" in message or "Calibrated Max Value:" in message or "Calibrated Range:" in message:
            self.response_text.append(f"  {message}")
        else:
            self.response_text.append(message)
            
        # Auto-scroll to bottom
        self.response_text.ensureCursorVisible()
        
    def clear_response(self):
        """Clear response text area"""
        self.response_text.clear()
        
    # Configuration methods
    def toggle_throttle(self):
        """Toggle throttle inversion"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            self.throttle_check.setChecked(not self.throttle_check.isChecked())
            return
            
        self.send_serial_command("invert_throttle")
        
    def toggle_level_assistant(self):
        """Toggle level assistant"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            self.level_assist_check.setChecked(not self.level_assist_check.isChecked())
            return
            
        self.send_serial_command("level_assistant")
        
    def toggle_speed_unit(self):
        """Toggle speed unit between km/h and mi/h"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            self.speed_unit_check.setChecked(not self.speed_unit_check.isChecked())
            return
            
        self.send_serial_command("toggle_speed_unit")
        
    def set_motor_pulley(self):
        """Set motor pulley teeth"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        teeth = self.pulley_spin.value()
        self.send_serial_command(f"set_motor_pulley {teeth}")
        
    def set_wheel_pulley(self):
        """Set wheel pulley teeth"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        teeth = self.wheel_pulley_spin.value()
        self.send_serial_command(f"set_wheel_pulley {teeth}")
        
    def set_wheel_size(self):
        """Set wheel diameter"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        size = self.wheel_spin.value()
        self.send_serial_command(f"set_wheel_size {size}")
        
    def set_motor_poles(self):
        """Set motor poles"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        poles = self.poles_spin.value()
        self.send_serial_command(f"set_motor_poles {poles}")
        
    def set_pid_kp(self):
        """Set PID Kp parameter"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        kp = self.pid_kp_spin.value()
        self.config['pid_kp'] = kp
        self.log_message(f"Setting PID Kp to {kp}...")
        self.send_serial_command(f"set_pid_kp {kp}")
        
    def set_pid_ki(self):
        """Set PID Ki parameter"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        ki = self.pid_ki_spin.value()
        self.config['pid_ki'] = ki
        self.log_message(f"Setting PID Ki to {ki}...")
        self.send_serial_command(f"set_pid_ki {ki}")
        
    def set_pid_kd(self):
        """Set PID Kd parameter"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        kd = self.pid_kd_spin.value()
        self.config['pid_kd'] = kd
        self.log_message(f"Setting PID Kd to {kd}...")
        self.send_serial_command(f"set_pid_kd {kd}")
        
    def set_pid_output_max(self):
        """Set PID Output Max parameter"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        output_max = self.pid_output_max_spin.value()
        self.config['pid_output_max'] = output_max
        self.log_message(f"Setting PID Output Max to {output_max}...")
        self.send_serial_command(f"set_pid_output_max {output_max}")
        
    def get_pid_params(self):
        """Get current PID parameters"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return
            
        self.log_message("Requesting PID parameters...")
        self.send_serial_command("get_pid_params")


    def load_pid_defaults(self):
        """Load default PID values into GUI"""
        self.pid_kp_spin.setValue(0.8)
        self.pid_ki_spin.setValue(0.5)
        self.pid_kd_spin.setValue(0.05)
        self.pid_output_max_spin.setValue(48.0)
        self.log_message("Loaded default PID values into GUI")

    def check_firmware_update(self):
        """Check for firmware updates"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        # First get the current firmware version from the device
        self.log_message("Checking firmware version...")
        self.send_serial_command("get_firmware_version")

        # Start a thread to check for updates after getting the version
        def check_updates():
            time.sleep(2)  # Wait for version response
            self.check_online_updates()

        thread = threading.Thread(target=check_updates, daemon=True)
        thread.start()

    def check_online_updates(self):
        """Check for updates online"""
        try:
            self.log_message("Checking for updates online...")

            # GitHub API endpoint for releases
            repo_url = "https://api.github.com/repos/georgebenett/gb_remote/releases/latest"

            # Get the latest release from GitHub API
            try:
                response = requests.get(repo_url, timeout=10)
                response.raise_for_status()
                release_data = response.json()
                latest_version = release_data['tag_name'].lstrip('v')  # Remove 'v' prefix if present
                self.log_message(f"[INFO] Latest release found: {latest_version}")
            except requests.exceptions.RequestException as e:
                self.log_message(f"[ERROR] Failed to fetch latest release: {str(e)}")
                return
            except KeyError as e:
                self.log_message(f"[ERROR] Invalid release data format: {str(e)}")
                return

            current_version = self.config.get('firmware_version', 'Unknown')

            if current_version == 'Unknown':
                self.log_message(f"[INFO] Current version: {current_version}")
                self.log_message(f"[INFO] Could not determine current firmware version")
                return

            try:
                # Compare versions using packaging library
                if version.parse(current_version) < version.parse(latest_version):
                    self.log_message(f"[UPDATE AVAILABLE] Current: {current_version}, Latest: {latest_version}")

                    # Show update dialog
                    result = QMessageBox.question(
                        self,
                        "Firmware Update Available",
                        f"New firmware version {latest_version} is available!\n\n"
                        f"Current version: {current_version}\n"
                        f"Latest version: {latest_version}\n\n"
                        "Would you like to download the update?",
                        QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                        QMessageBox.StandardButton.Yes
                    )

                    if result == QMessageBox.StandardButton.Yes:
                        self.download_firmware_update(latest_version)
                else:
                    self.log_message(f"[UP TO DATE] Current version {current_version} is the latest")

            except Exception as e:
                self.log_message(f"[ERROR] Version comparison failed: {str(e)}")

        except Exception as e:
            self.log_message(f"[ERROR] Update check failed: {str(e)}")

    def download_firmware_update(self, version):
        """Download firmware update"""
        try:
            self.log_message(f"[INFO] Preparing to download firmware version {version}...")

            # Get release information
            repo_url = "https://api.github.com/repos/georgebenett/gb_remote/releases/latest"
            response = requests.get(repo_url, timeout=10)
            response.raise_for_status()
            release_data = response.json()

            # Show release information
            release_name = release_data.get('name', f'Release {version}')
            release_notes = release_data.get('body', 'No release notes available')
            assets = release_data.get('assets', [])

            # Find firmware binary assets
            firmware_assets = [asset for asset in assets if asset['name'].endswith(('.bin', '.elf', '.zip'))]

            self.log_message(f"[INFO] Release: {release_name}")
            self.log_message(f"[INFO] Available assets: {len(firmware_assets)} files")
            for asset in firmware_assets:
                self.log_message(f"  - {asset['name']} ({asset['size']} bytes)")

            # Show download dialog with release information
            QMessageBox.information(
                self,
                "Firmware Update Available",
                f"Release: {release_name}\n"
                f"Version: {version}\n\n"
                f"Available files:\n" +
                "\n".join([f"• {asset['name']}" for asset in firmware_assets[:5]]) +
                (f"\n• ... and {len(firmware_assets)-5} more files" if len(firmware_assets) > 5 else "") +
                f"\n\nTo download and install:\n"
                f"1. Visit: https://github.com/georgebenett/gb_remote/releases/tag/{version}\n"
                f"2. Download the appropriate firmware file for your ESP32\n"
                f"3. Use ESP-IDF or your preferred flashing tool to install\n\n"
                f"Release Notes:\n{release_notes[:200]}{'...' if len(release_notes) > 200 else ''}"
            )

            self.log_message(f"[INFO] Update information displayed for version {version}")

        except Exception as e:
            self.log_message(f"[ERROR] Failed to get release information: {str(e)}")

    def detect_esp_idf_path(self):
        """Detect ESP-IDF installation path or esptool"""
        # First try to find esptool in PATH
        try:
            result = subprocess.run(['esptool', '--help'],
                                  capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                return "esptool"  # esptool is available in PATH
        except:
            pass

        # Try to find esptool via pip
        try:
            result = subprocess.run([sys.executable, '-m', 'esptool', '--help'],
                                  capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                return "pip_esptool"  # esptool available via pip
        except:
            pass

        # Common ESP-IDF installation paths
        possible_paths = [
            os.path.expanduser("~/esp/esp-idf"),
            os.path.expanduser("~/.espressif/esp-idf"),
            "/opt/esp/esp-idf",
            "/usr/local/esp/esp-idf",
            "C:\\Espressif\\frameworks\\esp-idf-v5.1.2",
            "C:\\Espressif\\frameworks\\esp-idf-v5.2",
        ]

        for path in possible_paths:
            if os.path.exists(path) and os.path.exists(os.path.join(path, "export.sh")):
                return path

        # Try to find from environment
        idf_path = os.environ.get('IDF_PATH')
        if idf_path and os.path.exists(idf_path):
            return idf_path

        return ""

    def browse_idf_path(self):
        """Browse for ESP-IDF installation path or esptool"""
        path = QFileDialog.getExistingDirectory(self, "Select ESP-IDF Installation Directory")
        if path:
            # Check if it's a valid ESP-IDF installation
            if os.path.exists(os.path.join(path, "export.sh")):
                self.idf_path_edit.setText(path)
                self.log_message(f"[INFO] ESP-IDF path set to: {path}")
            else:
                QMessageBox.critical(
                    self,
                    "Invalid ESP-IDF Path",
                    "Selected directory does not appear to be a valid ESP-IDF installation.\n"
                    "Please select a directory containing export.sh or install esptool via pip"
                )

    def browse_firmware_file(self):
        """Browse for firmware file"""
        file_path, _ = QFileDialog.getOpenFileName(
            self,
            "Select Firmware File",
            "",
            "Binary files (*.bin);;All files (*.*)"
        )
        if file_path:
            self.firmware_path_edit.setText(file_path)
            self.log_message(f"[INFO] Firmware file selected: {os.path.basename(file_path)}")

    def select_firmware_file(self):
        """Select firmware file using file dialog"""
        self.browse_firmware_file()

    def download_latest_firmware(self):
        """Download the latest firmware from GitHub releases"""
        try:
            self.log_message("[INFO] Downloading latest firmware...")

            # Get latest release information
            repo_url = "https://api.github.com/repos/georgebenett/gb_remote/releases/latest"
            response = requests.get(repo_url, timeout=10)
            response.raise_for_status()
            release_data = response.json()

            # Find firmware binary
            assets = release_data.get('assets', [])
            firmware_assets = [asset for asset in assets if asset['name'].endswith('.bin')]

            if not firmware_assets:
                self.log_message("[ERROR] No firmware binary found in latest release")
                return

            # Use the first firmware binary found
            firmware_asset = firmware_assets[0]
            download_url = firmware_asset['browser_download_url']
            filename = firmware_asset['name']

            self.log_message(f"[INFO] Downloading {filename}...")

            # Download the firmware
            response = requests.get(download_url, timeout=30)
            response.raise_for_status()

            # Save to temporary directory
            temp_dir = tempfile.mkdtemp(prefix="esp32_firmware_")
            firmware_path = os.path.join(temp_dir, filename)

            with open(firmware_path, 'wb') as f:
                f.write(response.content)

            self.firmware_path_edit.setText(firmware_path)
            self.log_message(f"[SUCCESS] Firmware downloaded to: {firmware_path}")

        except Exception as e:
            self.log_message(f"[ERROR] Failed to download firmware: {str(e)}")

    def flash_firmware(self):
        """Flash firmware to ESP32"""
        firmware_path = self.firmware_path_edit.text()
        idf_path = self.idf_path_edit.text()

        if not firmware_path or not os.path.exists(firmware_path):
            QMessageBox.critical(self, "Invalid Firmware", "Please select a valid firmware file first")
            return

        if not idf_path:
            QMessageBox.critical(self, "No ESP-IDF/esptool", "Please set ESP-IDF path or install esptool via pip")
            return

        # Check if it's a valid ESP-IDF installation or esptool
        if idf_path not in ["esptool", "pip_esptool"] and not os.path.exists(os.path.join(idf_path, "export.sh")):
            QMessageBox.critical(self, "Invalid ESP-IDF Path", "Please set a valid ESP-IDF installation path or install esptool via pip")
            return

        # Get the serial port
        port = self.port_combo.currentText()
        if not port:
            QMessageBox.critical(self, "No Port Selected", "Please select a serial port first")
            return

        # Confirm flashing
        result = QMessageBox.question(
            self,
            "Confirm Flashing",
            f"Are you sure you want to flash firmware to ESP32?\n\n"
            f"Port: {port}\n"
            f"Firmware: {os.path.basename(firmware_path)}\n\n"
            f"WARNING: This will overwrite the current firmware!",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No
        )

        if result != QMessageBox.StandardButton.Yes:
            return

        # Start flashing in a separate thread
        def flash_thread():
            self.flash_firmware_worker(port, firmware_path, idf_path)

        thread = threading.Thread(target=flash_thread, daemon=True)
        thread.start()

    def flash_firmware_worker(self, port, firmware_path, idf_path):
        """Worker function for flashing firmware"""
        try:
            self.flash_log_signal.emit(f"[INFO] Starting firmware flash...")
            self.flash_log_signal.emit(f"[INFO] Port: {port}")
            self.flash_log_signal.emit(f"[INFO] Firmware: {os.path.basename(firmware_path)}")

            # Disconnect from serial port if connected
            if self.serial_worker.is_connected:
                self.flash_log_signal.emit(f"[INFO] Disconnecting from serial port for flashing...")
                self.serial_worker.disconnect()
                time.sleep(2)  # Wait longer for port to be fully released

            # Set up environment
            env = os.environ.copy()

            # Flash offset for ESP32 firmware binaries
            # 0x10000 (64KB) is the standard offset for ESP32 application firmware
            flash_offset = "0x10000"

            # Determine esptool command based on detection method
            if idf_path == "esptool":
                # esptool is in PATH
                cmd = [
                    "esptool",
                    "--chip", "esp32c3",  # Adjust based on your target
                    "--port", port,
                    "--baud", "921600",
                    "--before", "default_reset",
                    "--after", "hard_reset",
                    "write_flash",
                    flash_offset, firmware_path
                ]
            elif idf_path == "pip_esptool":
                # esptool installed via pip
                cmd = [
                    sys.executable, "-m", "esptool",
                    "--chip", "esp32c3",  # Adjust based on your target
                    "--port", port,
                    "--baud", "921600",
                    "--before", "default_reset",
                    "--after", "hard_reset",
                    "write_flash",
                    flash_offset, firmware_path
                ]
            else:
                # ESP-IDF installation
                env['IDF_PATH'] = idf_path

                # Find esptool.py
                esptool_path = os.path.join(idf_path, "components", "esptool_py", "esptool.py")
                if not os.path.exists(esptool_path):
                    # Try alternative location
                    esptool_path = os.path.join(idf_path, "tools", "esptool_py", "esptool.py")

                if not os.path.exists(esptool_path):
                    self.flash_log_signal.emit(f"[ERROR] esptool.py not found in ESP-IDF installation")
                    return

                cmd = [
                    sys.executable, esptool_path,
                    "--chip", "esp32c3",  # Adjust based on your target
                    "--port", port,
                    "--baud", "921600",
                    "--before", "default_reset",
                    "--after", "hard_reset",
                    "write_flash",
                    flash_offset, firmware_path
                ]

            self.flash_log_signal.emit(f"[INFO] Flash offset: {flash_offset} (64KB)")
            self.flash_log_signal.emit(f"[INFO] Running: {' '.join(cmd)}")

            # Run esptool
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                env=env
            )

            # Stream output
            for line in iter(process.stdout.readline, ''):
                self.flash_log_signal.emit(f"[FLASH] {line.strip()}")

            process.wait()

            if process.returncode == 0:
                self.flash_log_signal.emit(f"[SUCCESS] Firmware flashed successfully!")
                self.flash_log_signal.emit(f"[INFO] ESP32 is resetting...")

                # Give the ESP32 time to reset
                for i in range(5, 0, -1):
                    self.flash_log_signal.emit(f"[INFO] Waiting for ESP32 reset... {i} seconds")
                    time.sleep(1)

                self.flash_log_signal.emit(f"[INFO] Ready to reconnect - click 'Connect' when ready")
                
                # Emit success signal instead of showing dialog directly
                self.flash_complete_signal.emit(True, "Firmware flashed successfully!")
            else:
                self.flash_log_signal.emit(f"[ERROR] Flashing failed with return code {process.returncode}")
                self.flash_complete_signal.emit(False, "Firmware flashing failed. Check the output for details.")

        except Exception as e:
            self.flash_log_signal.emit(f"[ERROR] Flashing error: {str(e)}")
            self.flash_complete_signal.emit(False, f"An error occurred during flashing:\n{str(e)}")
    
    def on_flash_complete(self, success, message):
        """Handle flash completion in main thread"""
        if success:
            QMessageBox.information(
                self,
                "Flash Complete",
                f"{message}\n\n"
                "The ESP32 is now resetting. Please:\n"
                "1. Wait 5-10 seconds for the device to restart\n"
                "2. Click 'Connect' to reconnect to the ESP32\n"
                "3. The new firmware should now be running"
            )
        else:
            QMessageBox.critical(self, "Flash Failed", message)

    def on_closing(self):
        """Handle window closing gracefully"""
        try:
            # Disconnect from serial port if connected
            if self.serial_worker.is_connected:
                self.serial_worker.disconnect()
            # Close the window
            self.close()
        except Exception as e:
            print(f"Error during window close: {e}")
            # Force close even if there are errors
            self.close()

    def reset_odometer(self):
        """Reset odometer"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        self.send_serial_command("reset_odometer")

    def get_config(self):
        """Get current configuration"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        self.log_message("Requesting current configuration...")
        self.send_serial_command("get_config")

    def show_help(self):
        """Show help information"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        self.send_serial_command("help")

    def calibrate_throttle(self):
        """Calibrate throttle"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        # Show a message box with instructions
        result = QMessageBox.question(
            self,
            "Throttle Calibration",
            "This will start a 6-second throttle calibration.\n\n"
            "IMPORTANT:\n"
            "- Move the throttle through its FULL range during calibration\n"
            "- Throttle signals will be set to neutral during calibration\n"
            "- Keep the throttle moving throughout the entire 6 seconds\n\n"
            "Do you want to proceed?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No
        )

        if result == QMessageBox.StandardButton.Yes:
            self.log_message("Starting throttle calibration...")
            self.send_serial_command("calibrate_throttle")

    def get_calibration(self):
        """Get throttle calibration status"""
        if not self.serial_worker.is_connected:
            QMessageBox.warning(self, "Not Connected", "Please connect to ESP32 first")
            return

        self.log_message("Requesting calibration status...")
        self.send_serial_command("get_calibration")

    def show_throttle_help(self):
        """Show throttle help tooltip"""
        QMessageBox.information(
            self,
            "Throttle Inversion Help",
            "Throttle Inversion controls how the throttle responds to your input:\n\n"
            "• INVERTED (like Boosted boards): Push thumb forward to brake, pull back to throttle\n"
            "• NORMAL: Pull back to brake, push forward to throttle\n\n"
            "Choose the style that feels most natural for your riding preference."
        )

    def show_level_assistant_help(self):
        """Show level assistant help tooltip"""
        QMessageBox.information(
            self,
            "Level Assistant Help",
            "Level Assistant is an electronic braking system that helps maintain the skateboard's position when you step off on a small hill. "
            "It automatically applies throttle to prevent the board from rolling away, keeping it stable until you get back on."
        )

    def show_speed_unit_help(self):
        """Show speed unit help tooltip"""
        QMessageBox.information(
            self,
            "Speed Unit Help",
            "• mi/h (miles per hour): For Americans who think 100 km/h sounds too fast\n"
            "• km/h (kilometers per hour): For the rest of the world who use the logical system"
        )

    def closeEvent(self, event):
        """Handle window close event"""
        self.on_closing()
        event.accept()

def main():
    app = QApplication(sys.argv)
    
    # Set application style
    app.setStyle('Fusion')
    
    # Create and show main window
    window = ESP32ControllerQt()
    window.show()
    
    # Start event loop
    sys.exit(app.exec())

if __name__ == "__main__":
    main()