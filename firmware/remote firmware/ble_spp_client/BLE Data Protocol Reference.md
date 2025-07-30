# BLE Data Protocol Reference

## Service UUID: 0xABF0
## Characteristic UUID: 0xABF2

## VESC Data Packet
Total size: 14 bytes (Big-endian format)

| Parameter | Bytes | Position | Type | Scale | Units |
|-----------|--------|-----------|------|---------|-------|
| temp_mos | 2 | 0-1 | int16_t | ÷100 | °C |
| temp_motor | 2 | 2-3 | int16_t | ÷100 | °C |
| current_motor | 2 | 4-5 | int16_t | ÷100 | A |
| current_in | 2 | 6-7 | int16_t | ÷100 | A |
| rpm | 4 | 8-11 | int32_t | none | RPM |
| voltage | 2 | 12-13 | int16_t | ÷100 | V |

## BMS Data Packet
Total size: 41 bytes (Big-endian format)

| Parameter | Bytes | Position | Type | Scale | Units |
|-----------|--------|-----------|------|---------|-------|
| total_voltage | 2 | 0-1 | int16_t | ÷100 | V |
| current | 2 | 2-3 | int16_t | ÷100 | A |
| remaining_capacity | 2 | 4-5 | int16_t | ÷100 | Ah |
| nominal_capacity | 2 | 6-7 | int16_t | ÷100 | Ah |
| num_cells | 1 | 8 | uint8_t | none | count |
| cell_voltages[16] | 32 | 9-40 | int16_t[] | ÷1000 | V |


