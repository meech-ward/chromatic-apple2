# SPDX-License-Identifier: MIT
# 33.554432 MHz board oscillator. All internal logic uses this one clock.
create_clock -name board_clk -period 29.802322 [get_ports {CLK_FPGA}]
