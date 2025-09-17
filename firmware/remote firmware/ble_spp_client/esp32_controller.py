#!/usr/bin/env python3
"""
ESP32 Hand Controller Configuration Interface
A GUI application to configure hand controller settings via USB serial
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog
import serial
import threading
import time
import queue
import sys
from datetime import datetime
import json
import requests
import re
from packaging import version
import subprocess
import os
import tempfile
import shutil

class ToolTip:
    """Create a tooltip for a given widget"""
    def __init__(self, widget, text='widget info'):
        self.widget = widget
        self.text = text
        self.tooltip_window = None
        self.widget.bind("<Enter>", self.enter)
        self.widget.bind("<Leave>", self.leave)

    def enter(self, event=None):
        self.show_tooltip()

    def leave(self, event=None):
        self.hide_tooltip()

    def show_tooltip(self):
        if self.tooltip_window or not self.text:
            return
        x, y, _, _ = self.widget.bbox("insert") if hasattr(self.widget, 'bbox') else (0, 0, 0, 0)
        x += self.widget.winfo_rootx() + 25
        y += self.widget.winfo_rooty() + 25
        self.tooltip_window = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry("+%d+%d" % (x, y))
        label = tk.Label(tw, text=self.text, justify='left',
                        background="#ffffe0", relief='solid', borderwidth=1,
                        font=("tahoma", "10", "normal"), wraplength=300)
        label.pack(ipadx=1)

    def hide_tooltip(self):
        tw = self.tooltip_window
        self.tooltip_window = None
        if tw:
            tw.destroy()

class HandControllerConfig:
    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Hand Controller Configuration")

        # Get screen dimensions and set responsive window size
        self.setup_responsive_window()

        # Serial connection
        self.serial_port = None
        self.is_connected = False
        self.response_queue = queue.Queue()

        # Configuration state
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
            # PID parameters
            'pid_kp': 0.8,
            'pid_ki': 0.5,
            'pid_kd': 0.05,
            'pid_output_max': 48.0
        }

        # Available commands
        self.commands = [
            "invert_throttle",
            "level_assistant",
            "reset_odometer",
            "toggle_speed_unit",
            "set_motor_pulley",
            "set_wheel_pulley",
            "set_wheel_size",
            "set_motor_poles",
            "get_config",
            "calibrate_throttle",
            "get_calibration",
            "set_pid_kp",
            "set_pid_ki",
            "set_pid_kd",
            "set_pid_output_max",
            "get_pid_params",
            "get_firmware_version",
            "help"
        ]

        self.setup_ui()
        self.start_response_thread()

        # Handle window closing gracefully
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        # Bind window resize event
        self.root.bind('<Configure>', self.on_window_resize)

    def setup_responsive_window(self):
        """Setup responsive window sizing based on screen resolution"""
        # Get screen dimensions
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()

        # Calculate window size as percentage of screen (max 90% of screen)
        window_width = min(int(screen_width * 0.9), 1500)  # Max 1500px width
        window_height = min(int(screen_height * 0.9), 1200)  # Max 1200px height

        # Set minimum window size
        window_width = max(window_width, 800)   # Minimum 800px width
        window_height = max(window_height, 600) # Minimum 600px height

        # Center the window on screen
        x = (screen_width - window_width) // 2
        y = (screen_height - window_height) // 2

        self.root.geometry(f"{window_width}x{window_height}+{x}+{y}")
        self.root.minsize(800, 600)  # Set minimum window size

        # Store dimensions for responsive calculations
        self.window_width = window_width
        self.window_height = window_height
        self.screen_width = screen_width
        self.screen_height = screen_height

    def on_window_resize(self, event):
        """Handle window resize events"""
        if event.widget == self.root:
            self.window_width = self.root.winfo_width()
            self.window_height = self.root.winfo_height()
            self.update_responsive_layout()

    def update_responsive_layout(self):
        """Update layout elements based on current window size"""
        # This method can be used to dynamically adjust layout elements
        # For now, we'll rely on the grid system's natural responsiveness
        pass

    def setup_ui(self):
        # Create main container with scrollbar
        self.create_scrollable_main()

        # Main frame
        main_frame = ttk.Frame(self.main_canvas, padding="10")
        self.main_canvas.create_window((0, 0), window=main_frame, anchor="nw")

        # Configure grid weights
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(1, weight=1)

        # Connection frame
        conn_frame = ttk.LabelFrame(main_frame, text="Connection", padding="5")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        conn_frame.columnconfigure(1, weight=1)

        # Port selection
        ttk.Label(conn_frame, text="Port:").grid(row=0, column=0, padx=(0, 5))
        self.port_var = tk.StringVar(value="/dev/ttyACM0")
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, width=15)
        self.port_combo['values'] = self.get_available_ports()
        self.port_combo.grid(row=0, column=1, padx=(0, 10), sticky='ew')

        # Connect button
        self.connect_btn = ttk.Button(conn_frame, text="Connect", command=self.toggle_connection)
        self.connect_btn.grid(row=0, column=2, padx=(0, 10))

        # Status label
        self.status_label = ttk.Label(conn_frame, text="Disconnected", foreground="red")
        self.status_label.grid(row=0, column=3)

        # Configuration frame
        config_frame = ttk.LabelFrame(main_frame, text="Hand Controller Configuration", padding="5")
        config_frame.grid(row=1, column=0, sticky=(tk.W, tk.E, tk.N, tk.S), padx=(0, 5))

        # Configure grid weights for consistent alignment
        config_frame.columnconfigure(0, weight=1)
        config_frame.columnconfigure(1, weight=1)
        config_frame.columnconfigure(2, weight=1)

        # Throttle inversion
        throttle_frame = ttk.Frame(config_frame)
        throttle_frame.grid(row=0, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        throttle_frame.columnconfigure(1, weight=1)

        # Throttle Inversion label with help icon
        throttle_label_frame = ttk.Frame(throttle_frame)
        throttle_label_frame.grid(row=0, column=0, padx=(0, 10), sticky='w')

        ttk.Label(throttle_label_frame, text="Throttle Inversion:", width=20, anchor='w').pack(side='left')

        # Help icon with tooltip
        throttle_help_label = tk.Label(throttle_label_frame, text="?",
                                      font=("Arial", 8, "bold"),
                                      fg="blue",
                                      cursor="hand2",
                                      relief="raised",
                                      bd=1,
                                      width=2,
                                      height=1)
        throttle_help_label.pack(side='left', padx=(5, 0))

        # Create tooltip for throttle help icon
        throttle_tooltip_text = ("Throttle Inversion controls how the throttle responds to your input:\n\n" +
                                "• INVERTED (like Boosted boards): Push thumb forward to brake, pull back to throttle\n" +
                                "• NORMAL: Pull back to brake, push forward to throttle\n\n" +
                                "Choose the style that feels most natural for your riding preference.")
        self.throttle_tooltip = ToolTip(throttle_help_label, throttle_tooltip_text)

        self.throttle_var = tk.BooleanVar(value=False)
        self.throttle_check = ttk.Checkbutton(throttle_frame, text="Inverted",
                                            variable=self.throttle_var,
                                            command=self.toggle_throttle)
        self.throttle_check.grid(row=0, column=1, sticky='w')

        # Level assistant configuration
        level_assist_frame = ttk.Frame(config_frame)
        level_assist_frame.grid(row=1, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        level_assist_frame.columnconfigure(1, weight=1)

        # Level Assistant label with help icon
        level_assist_label_frame = ttk.Frame(level_assist_frame)
        level_assist_label_frame.grid(row=0, column=0, padx=(0, 10), sticky='w')

        ttk.Label(level_assist_label_frame, text="Level Assistant:", width=20, anchor='w').pack(side='left')

        # Help icon with tooltip
        help_label = tk.Label(level_assist_label_frame, text="?",
                             font=("Arial", 8, "bold"),
                             fg="blue",
                             cursor="hand2",
                             relief="raised",
                             bd=1,
                             width=2,
                             height=1)
        help_label.pack(side='left', padx=(5, 0))

        # Create tooltip for help icon
        tooltip_text = ("Level Assistant is an electronic braking system that helps maintain the skateboard's position when you step off on a small hill. " +
                       "It automatically applies throttle to prevent the board from rolling away, keeping it stable until you get back on.")
        self.level_assist_tooltip = ToolTip(help_label, tooltip_text)

        self.level_assist_var = tk.BooleanVar(value=False)
        self.level_assist_check = ttk.Checkbutton(level_assist_frame, text="Enabled",
                                                variable=self.level_assist_var,
                                                command=self.toggle_level_assistant)
        self.level_assist_check.grid(row=0, column=1, sticky='w')

        speed_unit_frame = ttk.Frame(config_frame)
        speed_unit_frame.grid(row=2, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        speed_unit_frame.columnconfigure(1, weight=1)

        # Speed Unit label with help icon
        speed_unit_label_frame = ttk.Frame(speed_unit_frame)
        speed_unit_label_frame.grid(row=0, column=0, padx=(0, 10), sticky='w')

        ttk.Label(speed_unit_label_frame, text="Speed Unit:", width=20, anchor='w').pack(side='left')

        # Help icon with tooltip
        speed_unit_help_label = tk.Label(speed_unit_label_frame, text="?",
                                        font=("Arial", 8, "bold"),
                                        fg="blue",
                                        cursor="hand2",
                                        relief="raised",
                                        bd=1,
                                        width=2,
                                        height=1)
        speed_unit_help_label.pack(side='left', padx=(5, 0))

        # Create tooltip for speed unit help icon
        speed_unit_tooltip_text = ("• mi/h (miles per hour): For Americans who think 100 km/h sounds too fast\n" +
                                  "• km/h (kilometers per hour): For the rest of the world who use the logical system\n")
        self.speed_unit_tooltip = ToolTip(speed_unit_help_label, speed_unit_tooltip_text)

        self.speed_unit_var = tk.BooleanVar(value=False)
        self.speed_unit_check = ttk.Checkbutton(speed_unit_frame, text="mi/h (unchecked = km/h)",
                                              variable=self.speed_unit_var,
                                              command=self.toggle_speed_unit)
        self.speed_unit_check.grid(row=0, column=1, sticky='w')

        # Motor pulley configuration
        pulley_frame = ttk.Frame(config_frame)
        pulley_frame.grid(row=3, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        pulley_frame.columnconfigure(1, weight=1)

        ttk.Label(pulley_frame, text="Motor Pulley Teeth:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.pulley_var = tk.IntVar(value=15)
        self.pulley_entry = ttk.Entry(pulley_frame, textvariable=self.pulley_var, width=10)
        self.pulley_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(pulley_frame, text="Set", command=self.set_motor_pulley).grid(row=0, column=2, sticky='e')

        # Wheel pulley configuration
        wheel_pulley_frame = ttk.Frame(config_frame)
        wheel_pulley_frame.grid(row=3, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        wheel_pulley_frame.columnconfigure(1, weight=1)

        ttk.Label(wheel_pulley_frame, text="Wheel Pulley Teeth:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.wheel_pulley_var = tk.IntVar(value=33)
        self.wheel_pulley_entry = ttk.Entry(wheel_pulley_frame, textvariable=self.wheel_pulley_var, width=10)
        self.wheel_pulley_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(wheel_pulley_frame, text="Set", command=self.set_wheel_pulley).grid(row=0, column=2, sticky='e')

        # Wheel size configuration
        wheel_frame = ttk.Frame(config_frame)
        wheel_frame.grid(row=4, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        wheel_frame.columnconfigure(1, weight=1)

        ttk.Label(wheel_frame, text="Wheel Diameter (mm):", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.wheel_var = tk.IntVar(value=115)
        self.wheel_entry = ttk.Entry(wheel_frame, textvariable=self.wheel_var, width=10)
        self.wheel_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(wheel_frame, text="Set", command=self.set_wheel_size).grid(row=0, column=2, sticky='e')

        # Motor poles configuration
        poles_frame = ttk.Frame(config_frame)
        poles_frame.grid(row=5, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        poles_frame.columnconfigure(1, weight=1)

        ttk.Label(poles_frame, text="Motor Poles:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.poles_var = tk.IntVar(value=14)
        self.poles_entry = ttk.Entry(poles_frame, textvariable=self.poles_var, width=10)
        self.poles_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(poles_frame, text="Set", command=self.set_motor_poles).grid(row=0, column=2, sticky='e')

        # PID Tuning section
        pid_frame = ttk.LabelFrame(config_frame, text="Level Assistant PID Tuning", padding="5")
        pid_frame.grid(row=6, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=10)
        pid_frame.columnconfigure(0, weight=1)
        pid_frame.columnconfigure(1, weight=1)
        pid_frame.columnconfigure(2, weight=1)

        # PID Kp
        kp_frame = ttk.Frame(pid_frame)
        kp_frame.grid(row=0, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=2)
        kp_frame.columnconfigure(1, weight=1)
        ttk.Label(kp_frame, text="Kp (Proportional):", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.pid_kp_var = tk.DoubleVar(value=0.8)
        self.pid_kp_entry = ttk.Entry(kp_frame, textvariable=self.pid_kp_var, width=10)
        self.pid_kp_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(kp_frame, text="Set", command=self.set_pid_kp).grid(row=0, column=2, sticky='e')

        # PID Ki
        ki_frame = ttk.Frame(pid_frame)
        ki_frame.grid(row=1, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=2)
        ki_frame.columnconfigure(1, weight=1)
        ttk.Label(ki_frame, text="Ki (Integral):", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.pid_ki_var = tk.DoubleVar(value=0.5)
        self.pid_ki_entry = ttk.Entry(ki_frame, textvariable=self.pid_ki_var, width=10)
        self.pid_ki_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(ki_frame, text="Set", command=self.set_pid_ki).grid(row=0, column=2, sticky='e')

        # PID Kd
        kd_frame = ttk.Frame(pid_frame)
        kd_frame.grid(row=2, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=2)
        kd_frame.columnconfigure(1, weight=1)
        ttk.Label(kd_frame, text="Kd (Derivative):", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.pid_kd_var = tk.DoubleVar(value=0.05)
        self.pid_kd_entry = ttk.Entry(kd_frame, textvariable=self.pid_kd_var, width=10)
        self.pid_kd_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(kd_frame, text="Set", command=self.set_pid_kd).grid(row=0, column=2, sticky='e')

        # PID Output Max
        output_max_frame = ttk.Frame(pid_frame)
        output_max_frame.grid(row=3, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=2)
        output_max_frame.columnconfigure(1, weight=1)
        ttk.Label(output_max_frame, text="Output Max:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.pid_output_max_var = tk.DoubleVar(value=48.0)
        self.pid_output_max_entry = ttk.Entry(output_max_frame, textvariable=self.pid_output_max_var, width=10)
        self.pid_output_max_entry.grid(row=0, column=1, padx=(0, 10), sticky='w')
        ttk.Button(output_max_frame, text="Set", command=self.set_pid_output_max).grid(row=0, column=2, sticky='e')

        # PID Action buttons - make them wrap on smaller screens
        pid_actions_frame = ttk.Frame(pid_frame)
        pid_actions_frame.grid(row=4, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        pid_actions_frame.columnconfigure(0, weight=1)
        pid_actions_frame.columnconfigure(1, weight=1)

        ttk.Button(pid_actions_frame, text="Get PID Parameters", command=self.get_pid_params).grid(row=0, column=0, padx=5, sticky='ew')
        ttk.Button(pid_actions_frame, text="Load PID Defaults", command=self.load_pid_defaults).grid(row=0, column=1, padx=5, sticky='ew')

        # Firmware Flashing section
        flash_frame = ttk.LabelFrame(config_frame, text="Firmware Flashing", padding="5")
        flash_frame.grid(row=8, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=10)
        flash_frame.columnconfigure(0, weight=1)
        flash_frame.columnconfigure(1, weight=1)
        flash_frame.columnconfigure(2, weight=1)

        # ESP-IDF Path configuration
        idf_path_frame = ttk.Frame(flash_frame)
        idf_path_frame.grid(row=0, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        idf_path_frame.columnconfigure(1, weight=1)

        ttk.Label(idf_path_frame, text="ESP-IDF Path:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.idf_path_var = tk.StringVar(value=self.detect_esp_idf_path())
        self.idf_path_entry = ttk.Entry(idf_path_frame, textvariable=self.idf_path_var, width=40)
        self.idf_path_entry.grid(row=0, column=1, padx=(0, 10), sticky='ew')
        ttk.Button(idf_path_frame, text="Browse", command=self.browse_idf_path).grid(row=0, column=2, sticky='e')

        # Flashing actions - make them wrap on smaller screens
        flash_actions_frame = ttk.Frame(flash_frame)
        flash_actions_frame.grid(row=1, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        flash_actions_frame.columnconfigure(0, weight=1)
        flash_actions_frame.columnconfigure(1, weight=1)
        flash_actions_frame.columnconfigure(2, weight=1)

        ttk.Button(flash_actions_frame, text="Download Latest Firmware", command=self.download_latest_firmware).grid(row=0, column=0, padx=5, sticky='ew')
        ttk.Button(flash_actions_frame, text="Flash Firmware", command=self.flash_firmware).grid(row=0, column=1, padx=5, sticky='ew')
        ttk.Button(flash_actions_frame, text="Select Firmware File", command=self.select_firmware_file).grid(row=0, column=2, padx=5, sticky='ew')

        # Firmware file path
        firmware_path_frame = ttk.Frame(flash_frame)
        firmware_path_frame.grid(row=2, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        firmware_path_frame.columnconfigure(1, weight=1)

        ttk.Label(firmware_path_frame, text="Firmware File:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.firmware_path_var = tk.StringVar()
        self.firmware_path_entry = ttk.Entry(firmware_path_frame, textvariable=self.firmware_path_var, width=40)
        self.firmware_path_entry.grid(row=0, column=1, padx=(0, 10), sticky='ew')
        ttk.Button(firmware_path_frame, text="Browse", command=self.browse_firmware_file).grid(row=0, column=2, sticky='e')

        # Action buttons - make them wrap on smaller screens
        actions_frame = ttk.Frame(config_frame)
        actions_frame.grid(row=9, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=10)

        # Create a grid of buttons that will wrap
        action_buttons = [
            ("Reset Odometer", self.reset_odometer),
            ("Get Config", self.get_config),
            ("Calibrate Throttle", self.calibrate_throttle),
            ("Get Calibration", self.get_calibration),
            ("Check Firmware Update", self.check_firmware_update),
            ("Help", self.show_help)
        ]

        # Calculate number of columns based on window width
        buttons_per_row = max(2, min(6, self.window_width // 150))

        for i, (text, command) in enumerate(action_buttons):
            row = i // buttons_per_row
            col = i % buttons_per_row
            actions_frame.columnconfigure(col, weight=1)
            ttk.Button(actions_frame, text=text, command=command).grid(row=row, column=col, padx=5, pady=2, sticky='ew')

        # Response frame
        resp_frame = ttk.LabelFrame(main_frame, text="Response", padding="5")
        resp_frame.grid(row=1, column=1, rowspan=2, sticky=(tk.W, tk.E, tk.N, tk.S), padx=(5, 0))

        # Response text area
        self.response_text = scrolledtext.ScrolledText(resp_frame, height=20, width=50)
        self.response_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        # Clear button
        ttk.Button(resp_frame, text="Clear", command=self.clear_response).grid(row=1, column=0, pady=(5, 0))

        # Configure weights for response frame
        resp_frame.columnconfigure(0, weight=1)
        resp_frame.rowconfigure(0, weight=1)

        # Current configuration display
        config_display_frame = ttk.LabelFrame(main_frame, text="Current Configuration", padding="5")
        config_display_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(10, 0))

        self.setup_config_display(config_display_frame)

        # Update the scroll region after all widgets are created
        self.update_scroll_region()

    def create_scrollable_main(self):
        """Create the main scrollable container"""
        # Create main frame with canvas and scrollbar
        self.main_frame = ttk.Frame(self.root)
        self.main_frame.pack(fill=tk.BOTH, expand=True)

        # Create canvas and scrollbar
        self.main_canvas = tk.Canvas(self.main_frame)
        self.scrollbar = ttk.Scrollbar(self.main_frame, orient="vertical", command=self.main_canvas.yview)
        self.scrollable_frame = ttk.Frame(self.main_canvas)

        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: self.main_canvas.configure(scrollregion=self.main_canvas.bbox("all"))
        )

        self.main_canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")

        self.main_canvas.configure(yscrollcommand=self.scrollbar.set)

        self.main_canvas.pack(side="left", fill="both", expand=True)
        self.scrollbar.pack(side="right", fill="y")

        # Bind mousewheel to canvas
        def _on_mousewheel(event):
            self.main_canvas.yview_scroll(int(-1*(event.delta/120)), "units")
        self.main_canvas.bind_all("<MouseWheel>", _on_mousewheel)

    def update_scroll_region(self):
        """Update the scroll region of the canvas"""
        self.main_canvas.update_idletasks()
        self.main_canvas.configure(scrollregion=self.main_canvas.bbox("all"))

    def setup_config_display(self, parent):
        """Setup the configuration display widgets"""
        # Create labels for each config item
        self.config_labels = {}

        config_items = [
            ("Firmware Version", "firmware_version"),
            ("Throttle Inverted", "invert_throttle"),
            ("Level Assistant", "level_assistant"),
            ("Speed Unit", "speed_unit_mph"),  # Make sure this line is present
            ("Motor Pulley Teeth", "motor_pulley"),
            ("Wheel Pulley Teeth", "wheel_pulley"),
            ("Wheel Diameter (mm)", "wheel_diameter_mm"),
            ("Motor Poles", "motor_poles"),
            ("BLE Connected", "ble_connected"),
            ("PID Kp", "pid_kp"),
            ("PID Ki", "pid_ki"),
            ("PID Kd", "pid_kd"),
            ("PID Output Max", "pid_output_max")
        ]

        for i, (label, key) in enumerate(config_items):
            ttk.Label(parent, text=f"{label}:").grid(row=i, column=0, sticky=tk.W, padx=(0, 10))
            self.config_labels[key] = ttk.Label(parent, text="--")
            self.config_labels[key].grid(row=i, column=1, sticky=tk.W)

        # Configure weights
        parent.columnconfigure(1, weight=1)

    def update_config_display(self):
        """Update the configuration display"""
        if hasattr(self, 'config_labels'):
            self.config_labels['firmware_version'].config(
                text=str(self.config['firmware_version'])
            )
            self.config_labels['invert_throttle'].config(
                text="Yes" if self.config['invert_throttle'] else "No"
            )
            self.config_labels['level_assistant'].config(
                text="Yes" if self.config['level_assistant'] else "No"
            )
            self.config_labels['speed_unit_mph'].config(
                text="mi/h" if self.config['speed_unit_mph'] else "km/h"
            )
            self.config_labels['motor_pulley'].config(
                text=str(self.config['motor_pulley'])
            )
            self.config_labels['wheel_pulley'].config(
                text=str(self.config['wheel_pulley'])
            )
            self.config_labels['wheel_diameter_mm'].config(
                text=str(self.config['wheel_diameter_mm'])
            )
            self.config_labels['motor_poles'].config(
                text=str(self.config['motor_poles'])
            )
            self.config_labels['ble_connected'].config(
                text="Yes" if self.config['ble_connected'] else "No"
            )
            self.config_labels['pid_kp'].config(
                text=str(self.config['pid_kp'])
            )
            self.config_labels['pid_ki'].config(
                text=str(self.config['pid_ki'])
            )
            self.config_labels['pid_kd'].config(
                text=str(self.config['pid_kd'])
            )
            self.config_labels['pid_output_max'].config(
                text=str(self.config['pid_output_max'])
            )
            self.config_labels['speed_unit_mph'].config(
                text="mi/h" if self.config['speed_unit_mph'] else "km/h"
            )

    def get_available_ports(self):
        """Get list of available serial ports"""
        import glob
        if sys.platform.startswith('win'):
            ports = ['COM%s' % (i + 1) for i in range(256)]
        elif sys.platform.startswith('linux') or sys.platform.startswith('cygwin'):
            ports = glob.glob('/dev/tty[A-Za-z]*')
        elif sys.platform.startswith('darwin'):
            ports = glob.glob('/dev/tty.*')
        else:
            raise EnvironmentError('Unsupported platform')

        result = []
        for port in ports:
            try:
                s = serial.Serial(port)
                s.close()
                result.append(port)
            except (OSError, serial.SerialException):
                pass
        return result

    def toggle_connection(self):
        """Toggle serial connection"""
        if not self.is_connected:
            self.connect()
        else:
            self.disconnect()

    def connect(self):
        """Connect to ESP32"""
        try:
            port = self.port_var.get()
            self.serial_port = serial.Serial(
                port=port,
                baudrate=115200,
                timeout=1,
                write_timeout=1
            )
            self.is_connected = True
            self.connect_btn.config(text="Disconnect")
            self.status_label.config(text="Connected", foreground="green")
            self.log_message("Connected to ESP32 Hand Controller")

            # Start reading thread
            self.start_reading_thread()

        except Exception as e:
            messagebox.showerror("Connection Error", f"Failed to connect: {str(e)}")

    def disconnect(self):
        """Disconnect from ESP32"""
        if self.serial_port:
            try:
                self.serial_port.close()
            except Exception as e:
                print(f"Warning: Error closing serial port: {e}")
            finally:
                self.serial_port = None
        self.is_connected = False
        self.connect_btn.config(text="Connect")
        self.status_label.config(text="Disconnected", foreground="red")
        self.log_message("Disconnected from ESP32")

    def start_reading_thread(self):
        """Start thread to read from serial port"""
        def read_serial():
            while self.is_connected and self.serial_port:
                try:
                    if self.serial_port.in_waiting:
                        line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            self.response_queue.put(line)
                except (OSError, ValueError) as e:
                    # Handle "Bad file descriptor" and other serial errors gracefully
                    if "Bad file descriptor" in str(e) or "bad file descriptor" in str(e).lower():
                        # Port was closed, exit gracefully
                        break
                    else:
                        print(f"Serial read error: {e}")
                        break
                except Exception as e:
                    print(f"Unexpected serial read error: {e}")
                    break
                time.sleep(0.01)

        thread = threading.Thread(target=read_serial, daemon=True)
        thread.start()

    def start_response_thread(self):
        """Start thread to process responses"""
        def process_responses():
            while True:
                try:
                    response = self.response_queue.get(timeout=0.1)
                    self.process_response(response)
                except queue.Empty:
                    continue

        thread = threading.Thread(target=process_responses, daemon=True)
        thread.start()

    def toggle_throttle(self):
        """Toggle throttle inversion"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            # Reset checkbox to previous state if not connected
            self.throttle_var.set(not self.throttle_var.get())
            return

        self.send_serial_command("invert_throttle")

    def toggle_level_assistant(self):
        """Toggle level assistant"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            # Reset checkbox to previous state if not connected
            self.level_assist_var.set(not self.level_assist_var.get())
            return

        self.send_serial_command("level_assistant")

    def toggle_speed_unit(self):
        """Toggle speed unit between km/h and mi/h"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            # Reset checkbox to previous state if not connected
            self.speed_unit_var.set(not self.speed_unit_var.get())
            return

        self.send_serial_command("toggle_speed_unit")

    def reset_odometer(self):
        """Reset odometer"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        self.send_serial_command("reset_odometer")

    def set_motor_pulley(self):
        """Set motor pulley teeth"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            teeth = self.pulley_var.get()
            if teeth <= 0 or teeth > 255:
                messagebox.showerror("Invalid Value", "Pulley teeth must be between 1 and 255")
                return

            self.send_serial_command(f"set_motor_pulley {teeth}")
        except ValueError:
            messagebox.showerror("Invalid Value", "Please enter a valid number")

    def set_wheel_pulley(self):
        """Set wheel pulley teeth"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            teeth = self.wheel_pulley_var.get()
            if teeth <= 0 or teeth > 255:
                messagebox.showerror("Invalid Value", "Pulley teeth must be between 1 and 255")
                return

            self.send_serial_command(f"set_wheel_pulley {teeth}")
        except ValueError:
            messagebox.showerror("Invalid Value", "Please enter a valid number")

    def set_wheel_size(self):
        """Set wheel diameter"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            size = self.wheel_var.get()
            if size <= 0 or size > 255:
                messagebox.showerror("Invalid Value", "Wheel diameter must be between 1 and 255 mm")
                return

            self.send_serial_command(f"set_wheel_size {size}")
        except ValueError:
            messagebox.showerror("Invalid Value", "Please enter a valid number")

    def set_motor_poles(self):
        """Set motor poles"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            poles = self.poles_var.get()
            if poles <= 0 or poles > 255:
                messagebox.showerror("Invalid Value", "Motor poles must be between 1 and 255")
                return

            self.send_serial_command(f"set_motor_poles {poles}")
        except ValueError:
            messagebox.showerror("Invalid Value", "Please enter a valid number")

    def get_config(self):
        """Get current configuration"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        self.response_text.insert(tk.END, "Requesting current configuration...\n")
        self.response_text.see(tk.END)
        self.send_serial_command("get_config")

    def show_help(self):
        """Show help information"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        self.send_serial_command("help")

    def calibrate_throttle(self):
        """Calibrate throttle"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        # Show a message box with instructions
        result = messagebox.askyesno(
            "Throttle Calibration",
            "This will start a 6-second throttle calibration.\n\n"
            "IMPORTANT:\n"
            "- Move the throttle through its FULL range during calibration\n"
            "- Throttle signals will be set to neutral during calibration\n"
            "- Keep the throttle moving throughout the entire 6 seconds\n\n"
            "Do you want to proceed?"
        )

        if result:
            self.response_text.insert(tk.END, "Starting throttle calibration...\n")
            self.response_text.see(tk.END)
            self.send_serial_command("calibrate_throttle")

    def get_calibration(self):
        """Get throttle calibration status"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        self.response_text.insert(tk.END, "Requesting calibration status...\n")
        self.response_text.see(tk.END)
        self.send_serial_command("get_calibration")

    def set_pid_kp(self):
        """Set PID Kp parameter"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            kp = self.pid_kp_var.get()
            if kp < 0.0 or kp > 10.0:
                messagebox.showerror("Invalid Value", "Kp must be between 0.0 and 10.0")
                return

            self.config['pid_kp'] = kp
            self.response_text.insert(tk.END, f"Setting PID Kp to {kp}...\n")
            self.response_text.see(tk.END)
            self.send_serial_command(f"set_pid_kp {kp}")
        except Exception as e:
            messagebox.showerror("Error", f"Invalid Kp value: {e}")

    def set_pid_ki(self):
        """Set PID Ki parameter"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            ki = self.pid_ki_var.get()
            if ki < 0.0 or ki > 2.0:
                messagebox.showerror("Invalid Value", "Ki must be between 0.0 and 2.0")
                return

            self.config['pid_ki'] = ki
            self.response_text.insert(tk.END, f"Setting PID Ki to {ki}...\n")
            self.response_text.see(tk.END)
            self.send_serial_command(f"set_pid_ki {ki}")
        except Exception as e:
            messagebox.showerror("Error", f"Invalid Ki value: {e}")

    def set_pid_kd(self):
        """Set PID Kd parameter"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            kd = self.pid_kd_var.get()
            if kd < 0.0 or kd > 1.0:
                messagebox.showerror("Invalid Value", "Kd must be between 0.0 and 1.0")
                return

            self.config['pid_kd'] = kd
            self.response_text.insert(tk.END, f"Setting PID Kd to {kd}...\n")
            self.response_text.see(tk.END)
            self.send_serial_command(f"set_pid_kd {kd}")
        except Exception as e:
            messagebox.showerror("Error", f"Invalid Kd value: {e}")

    def set_pid_output_max(self):
        """Set PID Output Max parameter"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        try:
            output_max = self.pid_output_max_var.get()
            if output_max < 10.0 or output_max > 100.0:
                messagebox.showerror("Invalid Value", "Output Max must be between 10.0 and 100.0")
                return

            self.config['pid_output_max'] = output_max
            self.response_text.insert(tk.END, f"Setting PID Output Max to {output_max}...\n")
            self.response_text.see(tk.END)
            self.send_serial_command(f"set_pid_output_max {output_max}")
        except Exception as e:
            messagebox.showerror("Error", f"Invalid Output Max value: {e}")

    def get_pid_params(self):
        """Get current PID parameters"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        self.response_text.insert(tk.END, "Requesting PID parameters...\n")
        self.response_text.see(tk.END)
        self.send_serial_command("get_pid_params")

    def load_pid_defaults(self):
        """Load default PID values into GUI"""
        self.pid_kp_var.set(0.8)
        self.pid_ki_var.set(0.5)
        self.pid_kd_var.set(0.05)
        self.pid_output_max_var.set(48.0)
        self.response_text.insert(tk.END, "Loaded default PID values into GUI\n")
        self.response_text.see(tk.END)

    def check_firmware_update(self):
        """Check for firmware updates"""
        if not self.is_connected:
            messagebox.showwarning("Not Connected", "Please connect to ESP32 first")
            return

        # First get the current firmware version from the device
        self.response_text.insert(tk.END, "Checking firmware version...\n")
        self.response_text.see(tk.END)
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
            self.response_text.insert(tk.END, "Checking for updates online...\n")
            self.response_text.see(tk.END)

            # GitHub API endpoint for releases
            repo_url = "https://api.github.com/repos/georgebenett/gb_remote/releases/latest"

            # Get the latest release from GitHub API
            try:
                response = requests.get(repo_url, timeout=10)
                response.raise_for_status()
                release_data = response.json()
                latest_version = release_data['tag_name'].lstrip('v')  # Remove 'v' prefix if present
                self.response_text.insert(tk.END, f"[INFO] Latest release found: {latest_version}\n")
                self.response_text.see(tk.END)
            except requests.exceptions.RequestException as e:
                self.response_text.insert(tk.END, f"[ERROR] Failed to fetch latest release: {str(e)}\n")
                self.response_text.see(tk.END)
                return
            except KeyError as e:
                self.response_text.insert(tk.END, f"[ERROR] Invalid release data format: {str(e)}\n")
                self.response_text.see(tk.END)
                return

            current_version = self.config.get('firmware_version', 'Unknown')

            if current_version == 'Unknown':
                self.response_text.insert(tk.END, f"[INFO] Current version: {current_version}\n")
                self.response_text.insert(tk.END, f"[INFO] Could not determine current firmware version\n")
                self.response_text.see(tk.END)
                return

            try:
                # Compare versions using packaging library
                if version.parse(current_version) < version.parse(latest_version):
                    self.response_text.insert(tk.END, f"[UPDATE AVAILABLE] Current: {current_version}, Latest: {latest_version}\n")
                    self.response_text.see(tk.END)

                    # Show update dialog
                    result = messagebox.askyesno(
                        "Firmware Update Available",
                        f"New firmware version {latest_version} is available!\n\n"
                        f"Current version: {current_version}\n"
                        f"Latest version: {latest_version}\n\n"
                        "Would you like to download the update?",
                        icon='question'
                    )

                    if result:
                        self.download_firmware_update(latest_version)
                else:
                    self.response_text.insert(tk.END, f"[UP TO DATE] Current version {current_version} is the latest\n")
                    self.response_text.see(tk.END)

            except Exception as e:
                self.response_text.insert(tk.END, f"[ERROR] Version comparison failed: {str(e)}\n")
                self.response_text.see(tk.END)

        except Exception as e:
            self.response_text.insert(tk.END, f"[ERROR] Update check failed: {str(e)}\n")
            self.response_text.see(tk.END)

    def download_firmware_update(self, version):
        """Download firmware update"""
        try:
            self.response_text.insert(tk.END, f"[INFO] Preparing to download firmware version {version}...\n")
            self.response_text.see(tk.END)

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

            self.response_text.insert(tk.END, f"[INFO] Release: {release_name}\n")
            self.response_text.insert(tk.END, f"[INFO] Available assets: {len(firmware_assets)} files\n")
            for asset in firmware_assets:
                self.response_text.insert(tk.END, f"  - {asset['name']} ({asset['size']} bytes)\n")
            self.response_text.see(tk.END)

            # Show download dialog with release information
            messagebox.showinfo(
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

            self.response_text.insert(tk.END, f"[INFO] Update information displayed for version {version}\n")
            self.response_text.see(tk.END)

        except Exception as e:
            self.response_text.insert(tk.END, f"[ERROR] Failed to get release information: {str(e)}\n")
            self.response_text.see(tk.END)

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
        path = filedialog.askdirectory(title="Select ESP-IDF Installation Directory")
        if path:
            # Check if it's a valid ESP-IDF installation
            if os.path.exists(os.path.join(path, "export.sh")):
                self.idf_path_var.set(path)
                self.response_text.insert(tk.END, f"[INFO] ESP-IDF path set to: {path}\n")
                self.response_text.see(tk.END)
            else:
                messagebox.showerror("Invalid ESP-IDF Path",
                                   "Selected directory does not appear to be a valid ESP-IDF installation.\n"
                                   "Please select a directory containing export.sh or install esptool via pip")

    def browse_firmware_file(self):
        """Browse for firmware file"""
        file_path = filedialog.askopenfilename(
            title="Select Firmware File",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")]
        )
        if file_path:
            self.firmware_path_var.set(file_path)
            self.response_text.insert(tk.END, f"[INFO] Firmware file selected: {os.path.basename(file_path)}\n")
            self.response_text.see(tk.END)

    def select_firmware_file(self):
        """Select firmware file using file dialog"""
        self.browse_firmware_file()

    def download_latest_firmware(self):
        """Download the latest firmware from GitHub releases"""
        try:
            self.response_text.insert(tk.END, "[INFO] Downloading latest firmware...\n")
            self.response_text.see(tk.END)

            # Get latest release information
            repo_url = "https://api.github.com/repos/georgebenett/gb_remote/releases/latest"
            response = requests.get(repo_url, timeout=10)
            response.raise_for_status()
            release_data = response.json()

            # Find firmware binary
            assets = release_data.get('assets', [])
            firmware_assets = [asset for asset in assets if asset['name'].endswith('.bin')]

            if not firmware_assets:
                self.response_text.insert(tk.END, "[ERROR] No firmware binary found in latest release\n")
                self.response_text.see(tk.END)
                return

            # Use the first firmware binary found
            firmware_asset = firmware_assets[0]
            download_url = firmware_asset['browser_download_url']
            filename = firmware_asset['name']

            self.response_text.insert(tk.END, f"[INFO] Downloading {filename}...\n")
            self.response_text.see(tk.END)

            # Download the firmware
            response = requests.get(download_url, timeout=30)
            response.raise_for_status()

            # Save to temporary directory
            temp_dir = tempfile.mkdtemp(prefix="esp32_firmware_")
            firmware_path = os.path.join(temp_dir, filename)

            with open(firmware_path, 'wb') as f:
                f.write(response.content)

            self.firmware_path_var.set(firmware_path)
            self.response_text.insert(tk.END, f"[SUCCESS] Firmware downloaded to: {firmware_path}\n")
            self.response_text.see(tk.END)

        except Exception as e:
            self.response_text.insert(tk.END, f"[ERROR] Failed to download firmware: {str(e)}\n")
            self.response_text.see(tk.END)

    def flash_firmware(self):
        """Flash firmware to ESP32"""
        firmware_path = self.firmware_path_var.get()
        idf_path = self.idf_path_var.get()

        if not firmware_path or not os.path.exists(firmware_path):
            messagebox.showerror("Invalid Firmware", "Please select a valid firmware file first")
            return

        if not idf_path:
            messagebox.showerror("No ESP-IDF/esptool", "Please set ESP-IDF path or install esptool via pip")
            return

        # Check if it's a valid ESP-IDF installation or esptool
        if idf_path not in ["esptool", "pip_esptool"] and not os.path.exists(os.path.join(idf_path, "export.sh")):
            messagebox.showerror("Invalid ESP-IDF Path", "Please set a valid ESP-IDF installation path or install esptool via pip")
            return

        # Get the serial port
        port = self.port_var.get()
        if not port:
            messagebox.showerror("No Port Selected", "Please select a serial port first")
            return

        # Confirm flashing
        result = messagebox.askyesno(
            "Confirm Flashing",
            f"Are you sure you want to flash firmware to ESP32?\n\n"
            f"Port: {port}\n"
            f"Firmware: {os.path.basename(firmware_path)}\n\n"
            f"WARNING: This will overwrite the current firmware!"
        )

        if not result:
            return

        # Start flashing in a separate thread
        def flash_thread():
            self.flash_firmware_worker(port, firmware_path, idf_path)

        thread = threading.Thread(target=flash_thread, daemon=True)
        thread.start()

    def flash_firmware_worker(self, port, firmware_path, idf_path):
        """Worker function for flashing firmware"""
        try:
            self.response_text.insert(tk.END, f"[INFO] Starting firmware flash...\n")
            self.response_text.insert(tk.END, f"[INFO] Port: {port}\n")
            self.response_text.insert(tk.END, f"[INFO] Firmware: {os.path.basename(firmware_path)}\n")
            self.response_text.see(tk.END)

            # Disconnect from serial port if connected
            if self.is_connected:
                self.response_text.insert(tk.END, f"[INFO] Disconnecting from serial port for flashing...\n")
                self.response_text.see(tk.END)
                self.disconnect()
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
                    self.response_text.insert(tk.END, f"[ERROR] esptool.py not found in ESP-IDF installation\n")
                    self.response_text.see(tk.END)
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

            self.response_text.insert(tk.END, f"[INFO] Flash offset: {flash_offset} (64KB)\n")
            self.response_text.insert(tk.END, f"[INFO] Running: {' '.join(cmd)}\n")
            self.response_text.see(tk.END)

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
                self.response_text.insert(tk.END, f"[FLASH] {line.strip()}\n")
                self.response_text.see(tk.END)
                self.root.update()  # Update GUI

            process.wait()

            if process.returncode == 0:
                self.response_text.insert(tk.END, f"[SUCCESS] Firmware flashed successfully!\n")
                self.response_text.insert(tk.END, f"[INFO] ESP32 is resetting...\n")
                self.response_text.see(tk.END)

                # Give the ESP32 time to reset
                for i in range(5, 0, -1):
                    self.response_text.insert(tk.END, f"[INFO] Waiting for ESP32 reset... {i} seconds\n")
                    self.response_text.see(tk.END)
                    self.root.update()
                    time.sleep(1)

                self.response_text.insert(tk.END, f"[INFO] Ready to reconnect - click 'Connect' when ready\n")
                self.response_text.see(tk.END)

                # Show success dialog with reconnection instructions
                messagebox.showinfo(
                    "Flash Complete",
                    "Firmware flashed successfully!\n\n"
                    "The ESP32 is now resetting. Please:\n"
                    "1. Wait 5-10 seconds for the device to restart\n"
                    "2. Click 'Connect' to reconnect to the ESP32\n"
                    "3. The new firmware should now be running"
                )
            else:
                self.response_text.insert(tk.END, f"[ERROR] Flashing failed with return code {process.returncode}\n")
                self.response_text.see(tk.END)
                messagebox.showerror("Flash Failed", "Firmware flashing failed. Check the output for details.")

        except Exception as e:
            self.response_text.insert(tk.END, f"[ERROR] Flashing error: {str(e)}\n")
            self.response_text.see(tk.END)
            messagebox.showerror("Flash Error", f"An error occurred during flashing:\n{str(e)}")

    def on_closing(self):
        """Handle window closing gracefully"""
        try:
            # Disconnect from serial port if connected
            if self.is_connected:
                self.disconnect()
            # Close the window
            self.root.destroy()
        except Exception as e:
            print(f"Error during window close: {e}")
            # Force close even if there are errors
            self.root.destroy()

    def send_serial_command(self, command):
        """Send command via serial"""
        try:
            self.serial_port.write(f"{command}\n".encode())
            self.log_message(f"Sent: {command}")
        except Exception as e:
            messagebox.showerror("Send Error", f"Failed to send command: {str(e)}")

    def process_response(self, response):
        """Process response from ESP32"""
        # Clean up the response by removing ANSI color codes and verbose logging
        cleaned_response = self.clean_response(response)

        if cleaned_response:
            self.log_message(cleaned_response)

            # Parse configuration data
            self.parse_config_response(cleaned_response)

    def clean_response(self, response):
        """Clean up response by removing ANSI codes and verbose logging"""
        import re

        # Remove ANSI color codes
        ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
        cleaned = ansi_escape.sub('', response)

        # Remove verbose logging lines
        lines = cleaned.split('\n')
        filtered_lines = []

        for line in lines:
            line = line.strip()
            if not line:
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

            # Skip empty lines and just '>'
            if line in ['>', '']:
                continue

            filtered_lines.append(line)

        return '\n'.join(filtered_lines) if filtered_lines else None

    def parse_config_response(self, response):
        """Parse configuration response"""
        try:
            # Parse throttle inversion
            if "Throttle inversion: ENABLED" in response:
                self.config['invert_throttle'] = True
                self.throttle_var.set(True)
                self.update_config_display()
            elif "Throttle inversion: DISABLED" in response:
                self.config['invert_throttle'] = False
                self.throttle_var.set(False)
                self.update_config_display()

            # Parse level assistant
            if "Level assistant: ENABLED" in response:
                self.config['level_assistant'] = True
                self.level_assist_var.set(True)
                self.update_config_display()
            elif "Level assistant: DISABLED" in response:
                self.config['level_assistant'] = False
                self.level_assist_var.set(False)
                self.update_config_display()

            # Parse speed unit
            if "Speed Unit:" in response:
                self.config['speed_unit_mph'] = "mi/h" in response
                self.speed_unit_var.set(self.config['speed_unit_mph'])

            # Parse PID parameter responses
            if "PID Kp set to:" in response:
                try:
                    kp = float(response.split("PID Kp set to:")[1].strip())
                    self.config['pid_kp'] = kp
                    self.pid_kp_var.set(kp)
                    self.update_config_display()
                except:
                    pass

            if "PID Ki set to:" in response:
                try:
                    ki = float(response.split("PID Ki set to:")[1].strip())
                    self.config['pid_ki'] = ki
                    self.pid_ki_var.set(ki)
                    self.update_config_display()
                except:
                    pass

            if "PID Kd set to:" in response:
                try:
                    kd = float(response.split("PID Kd set to:")[1].strip())
                    self.config['pid_kd'] = kd
                    self.pid_kd_var.set(kd)
                    self.update_config_display()
                except:
                    pass

            if "PID Output Max set to:" in response:
                try:
                    output_max = float(response.split("PID Output Max set to:")[1].strip())
                    self.config['pid_output_max'] = output_max
                    self.pid_output_max_var.set(output_max)
                    self.update_config_display()
                except:
                    pass

            # Parse PID parameters display (from get_pid_params command)
            if "=== Level Assistant PID Parameters ===" in response:
                lines = response.split('\n')
                for line in lines:
                    if "Kp (Proportional):" in line:
                        try:
                            kp = float(line.split(":")[1].strip())
                            self.config['pid_kp'] = kp
                            self.pid_kp_var.set(kp)
                        except:
                            pass
                    elif "Ki (Integral):" in line:
                        try:
                            ki = float(line.split(":")[1].strip())
                            self.config['pid_ki'] = ki
                            self.pid_ki_var.set(ki)
                        except:
                            pass
                    elif "Kd (Derivative):" in line:
                        try:
                            kd = float(line.split(":")[1].strip())
                            self.config['pid_kd'] = kd
                            self.pid_kd_var.set(kd)
                        except:
                            pass
                    elif "Output Max:" in line:
                        try:
                            output_max = float(line.split(":")[1].strip())
                            self.config['pid_output_max'] = output_max
                            self.pid_output_max_var.set(output_max)
                        except:
                            pass
                self.update_config_display()

            # Parse motor pulley setting
            if "Motor pulley teeth set to:" in response:
                try:
                    teeth = int(response.split("Motor pulley teeth set to:")[1].strip())
                    self.config['motor_pulley'] = teeth
                    self.pulley_var.set(teeth)
                    self.update_config_display()
                except:
                    pass

            # Parse wheel pulley setting
            if "Wheel pulley teeth set to:" in response:
                try:
                    teeth = int(response.split("Wheel pulley teeth set to:")[1].strip())
                    self.config['wheel_pulley'] = teeth
                    self.wheel_pulley_var.set(teeth)
                    self.update_config_display()
                except:
                    pass

            # Parse wheel diameter setting
            if "Wheel diameter set to:" in response:
                try:
                    size = int(response.split("Wheel diameter set to:")[1].split("mm")[0].strip())
                    self.config['wheel_diameter_mm'] = size
                    self.wheel_var.set(size)
                    self.update_config_display()
                except:
                    pass

            # Parse motor poles setting
            if "Motor poles set to:" in response:
                try:
                    poles = int(response.split("Motor poles set to:")[1].strip())
                    self.config['motor_poles'] = poles
                    self.poles_var.set(poles)
                    self.update_config_display()
                except:
                    pass

            # Parse firmware version
            if "Firmware version:" in response or "Firmware Version:" in response:
                try:
                    version_text = response.split(":")[1].strip()
                    self.config['firmware_version'] = version_text
                    self.update_config_display()
                except:
                    pass

            # Parse configuration display - this handles the "get_config" response
            if "Current Configuration" in response or "Configuration:" in response:
                self.parse_config_display(response)
            # Also parse individual configuration lines
            elif any(keyword in response for keyword in [
                "Throttle Inverted:", "Motor Pulley Teeth:", "Wheel Pulley Teeth:",
                "Wheel Diameter:", "Motor Poles:", "BLE Connected:", "Firmware Version:"
            ]):
                self.parse_config_display(response)

        except Exception as e:
            print(f"Error parsing config response: {e}")

    def parse_config_display(self, response):
        """Parse the configuration display response"""
        lines = response.split('\n')

        for line in lines:
            if ':' in line:
                try:
                    key, value = line.split(':', 1)
                    key = key.strip()
                    value = value.strip()

                    # Handle different possible key names
                    if key in ["Firmware Version", "Firmware version"]:
                        self.config['firmware_version'] = value
                    elif key in ["Throttle Inverted", "Throttle inversion"]:
                        self.config['invert_throttle'] = (value.lower() in ["yes", "enabled", "true"])
                        self.throttle_var.set(self.config['invert_throttle'])
                    elif key in ["Level Assistant", "Level assistant"]:
                        self.config['level_assistant'] = (value.lower() in ["yes", "enabled", "true"])
                        self.level_assist_var.set(self.config['level_assistant'])
                    elif key in ["Speed Unit", "Speed unit"]:
                        self.config['speed_unit_mph'] = (value.lower() in ["mi/h", "true"])
                        self.speed_unit_var.set(self.config['speed_unit_mph'])
                    elif key in ["Motor Pulley Teeth", "Motor pulley teeth"]:
                        self.config['motor_pulley'] = int(value)
                        self.pulley_var.set(self.config['motor_pulley'])
                    elif key in ["Wheel Pulley Teeth", "Wheel pulley teeth"]:
                        self.config['wheel_pulley'] = int(value)
                        self.wheel_pulley_var.set(self.config['wheel_pulley'])
                    elif key in ["Wheel Diameter", "Wheel diameter"]:
                        # Handle "115 mm" format
                        diameter = value.split()[0] if ' ' in value else value
                        self.config['wheel_diameter_mm'] = int(diameter)
                        self.wheel_var.set(self.config['wheel_diameter_mm'])
                    elif key in ["Motor Poles", "Motor poles"]:
                        self.config['motor_poles'] = int(value)
                        self.poles_var.set(self.config['motor_poles'])
                    elif key in ["BLE Connected", "BLE connected"]:
                        self.config['ble_connected'] = (value.lower() in ["yes", "connected", "true"])

                    self.update_config_display()
                except Exception as e:
                    print(f"Error parsing line '{line}': {e}")
                    pass

    def log_message(self, message):
        """Add message to response text area"""
        # Format different types of messages
        if message.startswith("Sent:"):
            # Format sent commands
            self.response_text.insert(tk.END, f"  {message}\n")
        elif "set to:" in message:
            # Format configuration confirmations
            self.response_text.insert(tk.END, f"[OK] {message}\n")
        elif "Throttle inversion:" in message:
            # Format throttle status
            status = "enabled" if "ENABLED" in message else "disabled"
            self.response_text.insert(tk.END, f"[OK] Throttle inversion: {status}\n")
        elif "Level assistant:" in message:
            # Format level assistant status
            status = "enabled" if "ENABLED" in message else "disabled"
            self.response_text.insert(tk.END, f"[OK] Level assistant: {status}\n")
        elif "Odometer reset successfully" in message:
            # Format odometer reset - don't add duplicate since ESP32 already sends the message
            self.response_text.insert(tk.END, f"[OK] {message}\n")
        elif "Odometer reset" in message:
            # Format other odometer reset messages
            self.response_text.insert(tk.END, f"[OK] {message}\n")
        elif "Unknown command:" in message:
            # Format error messages
            self.response_text.insert(tk.END, f"[ERROR] {message}\n")
        elif "=== Hand Controller Commands ===" in message:
            # Format help header
            self.response_text.insert(tk.END, f"{message}\n")
        elif "help" in message and "Show this help message" in message:
            # Skip help command line
            pass
        elif any(cmd in message for cmd in ["invert_throttle", "level_assistant", "reset_odometer", "set_motor_pulley",
                                          "set_wheel_pulley", "set_wheel_size", "set_motor_poles", "get_config"]):
            # Format help command lines
            self.response_text.insert(tk.END, f"  {message}\n")
        elif "calibrate_throttle" in message and "Manually calibrate throttle range" in message:
            # Format calibration help line
            self.response_text.insert(tk.END, f"  {message}\n")
        elif "get_calibration" in message and "Show throttle calibration status" in message:
            # Format calibration help line
            self.response_text.insert(tk.END, f"  {message}\n")
        elif "Calibration progress:" in message:
            # Format calibration progress
            self.response_text.insert(tk.END, f"[PROGRESS] {message}\n")
        elif "Calibration complete!" in message:
            # Format calibration success
            self.response_text.insert(tk.END, f"[OK] {message}\n")
        elif "Calibration failed" in message:
            # Format calibration failure
            self.response_text.insert(tk.END, f"[ERROR] {message}\n")
        elif "Raw range:" in message or "Calibrated range:" in message:
            # Format calibration details
            self.response_text.insert(tk.END, f"  {message}\n")
        elif "Calibration Status:" in message:
            # Format calibration status
            self.response_text.insert(tk.END, f"[STATUS] {message}\n")
        elif "Current ADC Reading:" in message or "Current Mapped Value:" in message:
            # Format calibration details
            self.response_text.insert(tk.END, f"  {message}\n")
        elif "Throttle signals were set to neutral during calibration" in message:
            # Format calibration safety message
            self.response_text.insert(tk.END, f"[SAFETY] {message}\n")
        elif "Calibrated Min Value:" in message or "Calibrated Max Value:" in message or "Calibrated Range:" in message:
            # Format calibration values
            self.response_text.insert(tk.END, f"  {message}\n")
        else:
            # Default formatting
            self.response_text.insert(tk.END, f"{message}\n")

        self.response_text.see(tk.END)

    def clear_response(self):
        """Clear response text area"""
        self.response_text.delete(1.0, tk.END)

def main():
    import sys
    root = tk.Tk()
    app = HandControllerConfig(root)
    root.mainloop()

if __name__ == "__main__":
    main()