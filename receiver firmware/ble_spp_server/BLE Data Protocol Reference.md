# BLE Data Protocol Reference

## Service UUID: 0xABF0
## Characteristic UUID: 0xABF2

## Combined Telemetry Data Packet
Total size: 55 bytes (Big-endian format)

| Parameter | Bytes | Position | Type | Scale | Units |
|-----------|--------|-----------|------|---------|-------|
| temp_mos | 2 | 0-1 | int16_t | ÷100 | °C |
| temp_motor | 2 | 2-3 | int16_t | ÷100 | °C |
| current_motor | 2 | 4-5 | int16_t | ÷100 | A |
| current_in | 2 | 6-7 | int16_t | ÷100 | A |
| rpm | 4 | 8-11 | int32_t | none | RPM |
| voltage | 2 | 12-13 | int16_t | ÷100 | V |
| bms_total_voltage | 2 | 14-15 | int16_t | ÷100 | V |
| bms_current | 2 | 16-17 | int16_t | ÷100 | A |
| remaining_capacity | 2 | 18-19 | int16_t | ÷100 | Ah |
| nominal_capacity | 2 | 20-21 | int16_t | ÷100 | Ah |
| num_cells | 1 | 22 | uint8_t | none | count |
| cell_voltages[16] | 32 | 23-54 | int16_t[] | ÷1000 | V |


