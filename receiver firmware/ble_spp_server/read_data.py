import sys
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                           QGridLayout, QLabel, QFrame, QProgressBar, QSizePolicy, QHBoxLayout, QPushButton, QComboBox)
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtSerialPort import QSerialPort
from PyQt6.QtCharts import QChart, QChartView, QLineSeries, QValueAxis
from PyQt6.QtGui import QColor, QPalette
import json
import time
import os

class TelemetryDisplay(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("VESC & BMS Telemetry")

        # Initialize serial port
        self.serial = None

        # Set window flags to remove decorations
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)

        # Set fixed 16:9 size (1920x1080)
        self.setMinimumSize(1920, 1080)
        self.setMaximumSize(1920, 1080)
        self.setGeometry(0, 0, 1920, 1080)  # Position at (0,0) with 1920x1080 size

        # Create main widget and layout
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QVBoxLayout(main_widget)
        main_layout.setSpacing(0)
        main_layout.setContentsMargins(0, 0, 0, 0)

        # Create top bar with buttons
        top_bar = QWidget()
        top_bar.setFixedHeight(40)
        top_bar.setStyleSheet("background-color: #333333;")
        top_layout = QHBoxLayout(top_bar)
        top_layout.setContentsMargins(10, 0, 10, 0)

        # Add fullscreen button
        fullscreen_button = QPushButton("⛶")  # Unicode symbol for fullscreen
        fullscreen_button.setFixedSize(30, 30)
        fullscreen_button.setStyleSheet("""
            QPushButton {
                background-color: #333333;
                color: white;
                border: none;
                font-size: 20px;
            }
            QPushButton:hover {
                background-color: #555555;
            }
        """)
        fullscreen_button.clicked.connect(self.toggle_fullscreen)

        # Add close button
        close_button = QPushButton("×")
        close_button.setFixedSize(30, 30)
        close_button.setStyleSheet("""
            QPushButton {
                background-color: #333333;
                color: white;
                border: none;
                font-size: 20px;
            }
            QPushButton:hover {
                background-color: #ff0000;
            }
        """)
        close_button.clicked.connect(self.close)


        # Add buttons to layout
        top_layout.addStretch()
        top_layout.addWidget(fullscreen_button)
        top_layout.addWidget(close_button)

        # Add top bar to main layout
        main_layout.addWidget(top_bar)

        # Create content widget for charts and info
        content_widget = QWidget()
        content_layout = QHBoxLayout(content_widget)
        content_layout.setSpacing(0)
        content_layout.setContentsMargins(0, 0, 0, 0)

        # Create charts panel (left side)
        charts_widget = QWidget()
        charts_layout = QGridLayout(charts_widget)
        charts_layout.setSpacing(5)

        # Create info panel (right side)
        info_widget = QWidget()
        info_layout = QVBoxLayout(info_widget)
        info_layout.setContentsMargins(5, 5, 5, 5)

        # Set size policies
        charts_widget.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        info_widget.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Expanding)
        info_widget.setFixedWidth(400)

        # Create charts
        self.charts = {
            'vesc_current': self.create_chart("Current Draw (A)"),
            'vesc_rpm': self.create_chart("Motor RPM"),
            'bms_voltage': self.create_chart("BMS Total Voltage (V)"),
            'bms_current': self.create_chart("BMS Current (A)")
        }

        # Add charts to layout
        charts_layout.addWidget(self.charts['vesc_current'], 0, 0)
        charts_layout.addWidget(self.charts['vesc_rpm'], 0, 1)
        charts_layout.addWidget(self.charts['bms_voltage'], 1, 0)
        charts_layout.addWidget(self.charts['bms_current'], 1, 1)

        # Create info displays
        self.create_info_section(info_layout)

        # Add panels to content layout
        content_layout.addWidget(charts_widget, stretch=2)
        content_layout.addWidget(info_widget, stretch=1)

        # Add content widget to main layout
        main_layout.addWidget(content_widget)

        # Initialize time tracking and data buffer
        self.start_time = time.time()
        self.max_points = 30
        self.data_queue = []
        self.last_axis_update = 0
        self.last_info_update = 0

        # Setup update timer
        self.update_timer = QTimer()
        self.update_timer.timeout.connect(self.process_data_queue)
        self.update_timer.start(50)  # Update every 50ms to match ESP32

        # Show window in fullscreen
        self.showFullScreen()

    def create_info_section(self, parent_layout):
        # Create connection panel
        connection_frame = QFrame()
        connection_frame.setFrameStyle(QFrame.Shape.NoFrame)
        connection_layout = QVBoxLayout(connection_frame)
        connection_layout.addWidget(QLabel("<h2>Connection</h2>"))

        # Create port selection widget
        port_widget = QWidget()
        port_layout = QHBoxLayout(port_widget)
        port_layout.setContentsMargins(5, 5, 5, 5)

        # Create port dropdown
        self.port_combo = QComboBox()
        self.port_combo.setStyleSheet("""
            QComboBox {
                background-color: #f5f5f5;
                border: 1px solid #cccccc;
                border-radius: 3px;
                padding: 5px;
            }
        """)

        # Create connect button
        self.connect_button = QPushButton("Connect")
        self.connect_button.setStyleSheet("""
            QPushButton {
                background-color: #34c759;
                color: white;
                border: none;
                border-radius: 3px;
                padding: 5px 10px;
            }
            QPushButton:hover {
                background-color: #30b753;
            }
        """)

        # Create refresh button
        refresh_button = QPushButton("⟳")
        refresh_button.setFixedWidth(30)
        refresh_button.setStyleSheet("""
            QPushButton {
                background-color: #f5f5f5;
                border: 1px solid #cccccc;
                border-radius: 3px;
                padding: 5px;
            }
            QPushButton:hover {
                background-color: #e5e5e5;
            }
        """)

        # Add widgets to layout
        port_layout.addWidget(self.port_combo, stretch=1)
        port_layout.addWidget(self.connect_button)
        port_layout.addWidget(refresh_button)

        # Add port widget to connection layout
        connection_layout.addWidget(port_widget)

        # Connect signals
        self.connect_button.clicked.connect(self.toggle_connection)
        refresh_button.clicked.connect(self.refresh_ports)

        # Add connection frame to parent layout
        parent_layout.addWidget(connection_frame)

        # Create frames for different sections
        vesc_frame = QFrame()
        vesc_frame.setFrameStyle(QFrame.Shape.NoFrame)  # Remove frame
        vesc_layout = QVBoxLayout(vesc_frame)
        vesc_layout.addWidget(QLabel("<h2>VESC Data</h2>"))

        # VESC labels
        self.vesc_labels = {
            'voltage': QLabel("Voltage: --"),
            'current_motor': QLabel("Motor Current: --"),
            'current_input': QLabel("Input Current: --"),
            'rpm': QLabel("RPM: --"),
            'duty': QLabel("Duty Cycle: --"),
            'temp_mos': QLabel("MOSFET Temp: --"),
            'temp_motor': QLabel("Motor Temp: --"),
            'fault': QLabel("Fault: --")
        }

        for label in self.vesc_labels.values():
            vesc_layout.addWidget(label)

        # BMS frame
        bms_frame = QFrame()
        bms_frame.setFrameStyle(QFrame.Shape.NoFrame)  # Remove frame
        bms_layout = QVBoxLayout(bms_frame)
        bms_layout.addWidget(QLabel("<h2>BMS Data</h2>"))

        # BMS labels
        self.bms_labels = {
            'total_voltage': QLabel("Total Voltage: --"),
            'current': QLabel("Current: --"),
            'remaining_capacity': QLabel("Remaining Capacity: --"),
            'nominal_capacity': QLabel("Nominal Capacity: --"),
            'state_of_charge': QLabel("State of Charge: --%"),
        }

        for label in self.bms_labels.values():
            bms_layout.addWidget(label)

        # Cell voltages frame
        cells_frame = QFrame()
        cells_frame.setFrameStyle(QFrame.Shape.NoFrame)  # Remove frame
        cells_layout = QVBoxLayout(cells_frame)
        cells_layout.addWidget(QLabel("<h2>Cell Voltages</h2>"))

        # Create progress bars and labels for cells
        self.cell_bars = {}
        self.cell_labels = {}
        for i in range(16):
            cell_widget = QWidget()
            cell_layout = QGridLayout(cell_widget)
            cell_layout.setSpacing(2)
            cell_layout.setContentsMargins(2, 0, 2, 0)

            # Create label with fixed-width numbering and fixed width
            self.cell_labels[i] = QLabel(f"Cell {i+1:2d}")
            self.cell_labels[i].setFixedWidth(50)  # Set fixed width for all labels
            self.cell_labels[i].setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)  # Right align text
            cell_layout.addWidget(self.cell_labels[i], 0, 0)

            # Create progress bar
            bar = QProgressBar()
            bar.setMinimum(2800)  # 2.8V
            bar.setMaximum(4200)  # 4.2V
            bar.setTextVisible(True)
            bar.setFormat("%.3fV")  # Show voltage with 3 decimal places
            bar.setStyleSheet("""
                QProgressBar {
                    border: 1px solid #cccccc;
                    border-radius: 7px;
                    text-align: center;
                    height: 15px;
                    margin: 0px 5px;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI";
                    font-size: 11px;
                    font-weight: 500;
                    background-color: #f5f5f5;
                }
                QProgressBar::chunk {
                    background-color: #34c759;
                    border-radius: 6px;
                }
            """)
            self.cell_bars[i] = bar
            cell_layout.addWidget(bar, 0, 1)

            cells_layout.addWidget(cell_widget)

        # Add voltage statistics
        stats_widget = QWidget()
        stats_layout = QHBoxLayout(stats_widget)
        stats_layout.setSpacing(10)
        stats_layout.setContentsMargins(5, 10, 5, 5)

        self.voltage_stats = {
            'min': QLabel("Min: --V"),
            'max': QLabel("Max: --V"),
            'delta': QLabel("ΔV: --V")
        }

        # Style the labels
        for label in self.voltage_stats.values():
            label.setStyleSheet("""
                QLabel {
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI";
                    font-size: 12px;
                    font-weight: 500;
                    color: #333333;
                    padding: 5px;
                    background-color: #f5f5f5;
                    border-radius: 5px;
                }
            """)
            stats_layout.addWidget(label)

        cells_layout.addWidget(stats_widget)

        # Add all frames to parent layout
        parent_layout.addWidget(vesc_frame)
        parent_layout.addWidget(bms_frame)
        parent_layout.addWidget(cells_frame)
        parent_layout.addStretch()

    def update_info_displays(self, data):
        # Update VESC info
        vesc = data['vesc']
        for key, label in self.vesc_labels.items():
            if key in vesc:
                value = vesc[key]
                if isinstance(value, float):
                    label.setText(f"{key.replace('_', ' ').title()}: {value:.2f}")
                else:
                    label.setText(f"{key.replace('_', ' ').title()}: {value}")

        # Update BMS info
        bms = data['bms']
        # Calculate SoC first
        if 'remaining_capacity' in bms and 'nominal_capacity' in bms:
            soc = (bms['remaining_capacity'] / bms['nominal_capacity']) * 100
            bms['state_of_charge'] = soc  # Add SoC to the data structure

        for key, label in self.bms_labels.items():
            if key == 'state_of_charge':
                label.setText(f"State of Charge: {bms['state_of_charge']:.1f}%")
            elif key in bms:
                value = bms[key]
                label.setText(f"{key.replace('_', ' ').title()}: {value:.2f}")

        # Update cell voltage bars
        if 'cell_voltages' in data['bms']:
            for i, voltage in enumerate(data['bms']['cell_voltages']):
                if i in self.cell_bars:
                    # Convert voltage to millivolts for the progress bar
                    mv = int(voltage * 1000)
                    self.cell_bars[i].setValue(mv)
                    self.cell_bars[i].setFormat(f"{voltage:.3f}V")

                    # Color the bar based on voltage
                    if voltage < 3.0:  # Low voltage
                        self.cell_bars[i].setStyleSheet("""
                            QProgressBar {
                                border: 1px solid #cccccc;
                                border-radius: 7px;
                                text-align: center;
                                height: 15px;
                                margin: 0px 5px;
                                font-family: -apple-system, BlinkMacSystemFont, "Segoe UI";
                                font-size: 11px;
                                font-weight: 500;
                                background-color: #f5f5f5;
                            }
                            QProgressBar::chunk {
                                background-color: #ff3b30;
                                border-radius: 6px;
                            }
                        """)
                    elif voltage > 4.1:  # High voltage
                        self.cell_bars[i].setStyleSheet("""
                            QProgressBar {
                                border: 1px solid #cccccc;
                                border-radius: 7px;
                                text-align: center;
                                height: 15px;
                                margin: 0px 5px;
                                font-family: -apple-system, BlinkMacSystemFont, "Segoe UI";
                                font-size: 11px;
                                font-weight: 500;
                                background-color: #f5f5f5;
                            }
                            QProgressBar::chunk {
                                background-color: #ffcc00;
                                border-radius: 6px;
                            }
                        """)
                    else:  # Normal voltage
                        self.cell_bars[i].setStyleSheet("""
                            QProgressBar {
                                border: 1px solid #cccccc;
                                border-radius: 7px;
                                text-align: center;
                                height: 15px;
                                margin: 0px 5px;
                                font-family: -apple-system, BlinkMacSystemFont, "Segoe UI";
                                font-size: 11px;
                                font-weight: 500;
                                background-color: #f5f5f5;
                            }
                            QProgressBar::chunk {
                                background-color: #34c759;
                                border-radius: 6px;
                            }
                        """)

            # Update voltage statistics
            voltages = data['bms']['cell_voltages']
            min_v = min(voltages)
            max_v = max(voltages)
            delta = max_v - min_v
            delta_mv = delta * 1000  # Convert to millivolts

            self.voltage_stats['min'].setText(f"Min: {min_v:.3f}V")
            self.voltage_stats['max'].setText(f"Max: {max_v:.3f}V")
            self.voltage_stats['delta'].setText(f"ΔV: {delta_mv:.0f}mV")

    def read_data(self):
        """Read data from the serial port"""
        if not self.serial or not self.serial.isOpen():
            return

        while self.serial.canReadLine():
            try:
                line = self.serial.readLine().data().decode().strip()
                data = json.loads(line)
                self.data_queue.append(data)
            except (json.JSONDecodeError, UnicodeDecodeError) as e:
                print(f"Error reading data: {e}")
                continue

    def process_data_queue(self):
        if not self.serial or not self.serial.isOpen():
            return

        if not self.data_queue:
            return

        current_time = time.time() - self.start_time

        # Process all available data points
        for data in self.data_queue:
            # Update charts
            self.update_series(self.charts['vesc_current'].chart().series()[0], current_time, data['vesc']['current_motor'])
            self.update_series(self.charts['vesc_rpm'].chart().series()[0], current_time, data['vesc']['rpm'])
            self.update_series(self.charts['bms_voltage'].chart().series()[0], current_time, data['bms']['total_voltage'])
            self.update_series(self.charts['bms_current'].chart().series()[0], current_time, data['bms']['current'])

        self.data_queue.clear()

        # Update axes every 50ms (same as data rate)
        if current_time - self.last_axis_update >= 0.05:
            self.last_axis_update = current_time
            for chart_view in self.charts.values():
                chart = chart_view.chart()
                axis_x = chart.axes(Qt.Orientation.Horizontal)[0]
                axis_x.setRange(current_time - 30, current_time)

        # Update info displays every 50ms
        if current_time - self.last_info_update >= 0.05:
            self.last_info_update = current_time
            self.update_info_displays(data)

    def update_series(self, series, time, value):
        series.append(time, value)
        if series.count() > self.max_points:
            series.remove(0)

        # Get the chart and y-axis
        chart = series.chart()
        axis_y = chart.axes(Qt.Orientation.Vertical)[0]

        # Find min and max values in visible data
        points = [series.at(i).y() for i in range(series.count())]
        if points:
            min_val = min(points)
            max_val = max(points)

            # Add 10% padding to the range
            padding = (max_val - min_val) * 0.1
            axis_y.setRange(min_val - padding, max_val + padding)

    def create_chart(self, title):
        chart = QChart()
        chart.setTitle(title)
        series = QLineSeries()
        chart.addSeries(series)

        axis_x = QValueAxis()
        axis_x.setTitleText("Time (s)")
        axis_x.setRange(0, 30)

        axis_y = QValueAxis()
        axis_y.setTitleText(title.split('(')[0].strip())
        axis_y.setTickCount(5)

        # Add axes to chart first
        chart.addAxis(axis_x, Qt.AlignmentFlag.AlignBottom)
        chart.addAxis(axis_y, Qt.AlignmentFlag.AlignLeft)

        # Attach series to axes
        series.attachAxis(axis_x)
        series.attachAxis(axis_y)

        # Now we can set the initial range and apply nice numbers
        axis_y.setRange(0, 1)  # Initial range, will auto-adjust
        axis_y.applyNiceNumbers()

        view = QChartView(chart)
        view.setMinimumSize(400, 300)
        return view

    def toggle_fullscreen(self):
        if self.isFullScreen():
            self.showNormal()
            self.setGeometry(0, 0, 1920, 1080)
        else:
            self.showFullScreen()

    def create_port_selector(self):
        """Create a port selector widget"""
        port_widget = QWidget()
        port_layout = QHBoxLayout(port_widget)
        port_layout.setContentsMargins(5, 0, 5, 0)

        # Create port dropdown
        self.port_combo = QComboBox()
        self.port_combo.setFixedWidth(120)
        self.port_combo.setStyleSheet("""
            QComboBox {
                background-color: #444444;
                color: white;
                border: 1px solid #555555;
                border-radius: 3px;
                padding: 5px;
            }
            QComboBox::drop-down {
                border: none;
            }
            QComboBox::down-arrow {
                image: url(down_arrow.png);
                width: 12px;
                height: 12px;
            }
        """)

        # Create connect button
        self.connect_button = QPushButton("Connect")
        self.connect_button.setFixedWidth(80)
        self.connect_button.setStyleSheet("""
            QPushButton {
                background-color: #444444;
                color: white;
                border: 1px solid #555555;
                border-radius: 3px;
                padding: 5px;
            }
            QPushButton:hover {
                background-color: #666666;
            }
        """)

        # Create refresh button
        refresh_button = QPushButton("⟳")
        refresh_button.setFixedWidth(30)
        refresh_button.setStyleSheet("""
            QPushButton {
                background-color: #444444;
                color: white;
                border: 1px solid #555555;
                border-radius: 3px;
                padding: 5px;
            }
            QPushButton:hover {
                background-color: #666666;
            }
        """)

        # Add widgets to layout
        port_layout.addWidget(self.port_combo)
        port_layout.addWidget(self.connect_button)
        port_layout.addWidget(refresh_button)

        # Connect signals
        self.connect_button.clicked.connect(self.toggle_connection)
        refresh_button.clicked.connect(self.refresh_ports)

        return port_widget

    def refresh_ports(self):
        """Refresh the available ports list"""
        self.port_combo.clear()
        for i in range(4):  # Check USB0 through USB3
            port_name = f'/dev/ttyUSB{i}'
            if os.path.exists(port_name):
                self.port_combo.addItem(port_name)

    def toggle_connection(self):
        """Handle connection/disconnection"""
        if self.serial and self.serial.isOpen():
            # Disconnect
            self.serial.close()
            self.serial = None
            self.connect_button.setText("Connect")
            self.connect_button.setStyleSheet("""
                QPushButton {
                    background-color: #34c759;
                    color: white;
                    border: none;
                    border-radius: 3px;
                    padding: 5px 10px;
                }
                QPushButton:hover {
                    background-color: #30b753;
                }
            """)
        else:
            # Connect
            port_name = self.port_combo.currentText()
            if port_name:
                self.serial = QSerialPort()
                self.serial.setPortName(port_name)
                self.serial.setBaudRate(115200)
                self.serial.readyRead.connect(self.read_data)

                if self.serial.open(QSerialPort.OpenModeFlag.ReadOnly):
                    self.connect_button.setText("Disconnect")
                    self.connect_button.setStyleSheet("""
                        QPushButton {
                            background-color: #ff3b30;
                            color: white;
                            border: none;
                            border-radius: 3px;
                            padding: 5px;
                        }
                        QPushButton:hover {
                            background-color: #ff453a;
                        }
                    """)
                else:
                    self.serial = None
                    print(f"Failed to open {port_name}")

if __name__ == '__main__':
    app = QApplication(sys.argv)
    window = TelemetryDisplay()
    sys.exit(app.exec())