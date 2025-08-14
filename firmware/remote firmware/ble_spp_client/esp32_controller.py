#!/usr/bin/env python3
"""
ESP32 Hand Controller Configuration Interface
A GUI application to configure hand controller settings via USB serial
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import serial
import threading
import time
import queue
import sys
from datetime import datetime
import json

class HandControllerConfig:
    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Hand Controller Configuration")
        self.root.geometry("1100x800")
        
        # Serial connection
        self.serial_port = None
        self.is_connected = False
        self.response_queue = queue.Queue()
        
        # Configuration state
        self.config = {
            'invert_throttle': False,
            'level_assistant': False,
            'motor_pulley': 15,
            'wheel_pulley': 33,
            'wheel_diameter_mm': 115,
            'motor_poles': 14,
            'ble_connected': False,
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
            "help"
        ]
        
        self.setup_ui()
        self.start_response_thread()
    
    def setup_ui(self):
        # Main frame
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Configure grid weights
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(1, weight=1)
        
        # Connection frame
        conn_frame = ttk.LabelFrame(main_frame, text="Connection", padding="5")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        
        # Port selection
        ttk.Label(conn_frame, text="Port:").grid(row=0, column=0, padx=(0, 5))
        self.port_var = tk.StringVar(value="/dev/ttyACM0")
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, width=15)
        self.port_combo['values'] = self.get_available_ports()
        self.port_combo.grid(row=0, column=1, padx=(0, 10))
        
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
        
        ttk.Label(throttle_frame, text="Throttle Inversion:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.throttle_var = tk.BooleanVar(value=False)
        self.throttle_check = ttk.Checkbutton(throttle_frame, text="Inverted", 
                                            variable=self.throttle_var, 
                                            command=self.toggle_throttle)
        self.throttle_check.grid(row=0, column=1, sticky='w')
        
        # Level assistant configuration
        level_assist_frame = ttk.Frame(config_frame)
        level_assist_frame.grid(row=1, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        level_assist_frame.columnconfigure(1, weight=1)
        
        ttk.Label(level_assist_frame, text="Level Assistant:", width=20, anchor='w').grid(row=0, column=0, padx=(0, 10), sticky='w')
        self.level_assist_var = tk.BooleanVar(value=False)
        self.level_assist_check = ttk.Checkbutton(level_assist_frame, text="Enabled", 
                                                variable=self.level_assist_var, 
                                                command=self.toggle_level_assistant)
        self.level_assist_check.grid(row=0, column=1, sticky='w')
        
        # Motor pulley configuration
        pulley_frame = ttk.Frame(config_frame)
        pulley_frame.grid(row=2, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
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
        
        # PID Action buttons
        pid_actions_frame = ttk.Frame(pid_frame)
        pid_actions_frame.grid(row=4, column=0, columnspan=3, sticky=(tk.W, tk.E), pady=5)
        ttk.Button(pid_actions_frame, text="Get PID Parameters", command=self.get_pid_params).grid(row=0, column=0, padx=5)
        ttk.Button(pid_actions_frame, text="Load PID Defaults", command=self.load_pid_defaults).grid(row=0, column=1, padx=5)
        
        # Action buttons
        actions_frame = ttk.Frame(config_frame)
        actions_frame.grid(row=7, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=10)
        
        ttk.Button(actions_frame, text="Reset Odometer", command=self.reset_odometer).grid(row=0, column=0, padx=5)
        ttk.Button(actions_frame, text="Get Config", command=self.get_config).grid(row=0, column=1, padx=5)
        ttk.Button(actions_frame, text="Calibrate Throttle", command=self.calibrate_throttle).grid(row=0, column=2, padx=5)
        ttk.Button(actions_frame, text="Get Calibration", command=self.get_calibration).grid(row=0, column=3, padx=5)
        ttk.Button(actions_frame, text="Help", command=self.show_help).grid(row=0, column=4, padx=5)
        
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
    
    def setup_config_display(self, parent):
        """Setup the configuration display widgets"""
        # Create labels for each config item
        self.config_labels = {}
        
        config_items = [
            ("Throttle Inverted", "invert_throttle"),
            ("Level Assistant", "level_assistant"),
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
            self.config_labels['invert_throttle'].config(
                text="Yes" if self.config['invert_throttle'] else "No"
            )
            self.config_labels['level_assistant'].config(
                text="Yes" if self.config['level_assistant'] else "No"
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
            self.serial_port.close()
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
                except Exception as e:
                    print(f"Serial read error: {e}")
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
            
            # Parse configuration display - this handles the "get_config" response
            if "Current Configuration" in response or "Configuration:" in response:
                self.parse_config_display(response)
            # Also parse individual configuration lines
            elif any(keyword in response for keyword in [
                "Throttle Inverted:", "Motor Pulley Teeth:", "Wheel Pulley Teeth:", 
                "Wheel Diameter:", "Motor Poles:", "BLE Connected:"
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
                    if key in ["Throttle Inverted", "Throttle inversion"]:
                        self.config['invert_throttle'] = (value.lower() in ["yes", "enabled", "true"])
                        self.throttle_var.set(self.config['invert_throttle'])
                    elif key in ["Level Assistant", "Level assistant"]:
                        self.config['level_assistant'] = (value.lower() in ["yes", "enabled", "true"])
                        self.level_assist_var.set(self.config['level_assistant'])
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