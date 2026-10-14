// SPDX-License-Identifier: MIT
// Original Apple II board controller. No Game Boy core, USB PHY or vendor IP.
// CART_MODE=0 is the mandatory first-upload default until board straps are checked.
module chromatic_a2_top #(
    parameter CLOCK_HZ=33554432, parameter CART_MODE=0,
    parameter FLASH_WRITES=0, parameter UART_BAUD=115200
) (
    input wire CLK_FPGA, input wire POWER_ON_FPGA,
    input wire VERSION_DET, input wire VERSION_DET2, input wire DISPLAY_ID,
    input wire BTN_A, BTN_B, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
    input wire BTN_DPAD_UP, BTN_MENU, BTN_SEL, BTN_START,
    input wire ESP32_MCU_D12, output wire ESP32_MCU_D11,
    output wire ESP32_EN, ESP32_IO0, ESP32_MCU_D4,
    input wire ESP32_MCU_D3,
    input wire QSPI_CLK, QSPI_CS, QSPI_MOSI, QSPI_MISO, QSPI_WP, QSPI_HD,
    output wire I2S_BCLK,I2S_WS, input wire I2S_DIN,I2S_DOUT,
    output wire [15:0] CART_A, inout wire [7:0] CART_D,
    output wire CART_CLK, CART_CS, CART_RD, CART_WR, CART_DATA_DIR_E,
    output wire CART_CTRL_OE, CART_PWR_EN, input wire CART_DET,
    inout wire CART_RST,
    output wire [5:0] LCD_DB,
    output wire LCD_DOTCLK, LCD_ENABLE, LCD_HSYNC, LCD_VSYNC,
    output wire LCD_RESET, LCD_SPI_CSX, LCD_SPI_SCLK, LCD_PWM,
    inout wire LCD_SPI_SDA,
    input wire LCD_TE,
    output wire FPGA_LED_EN, FPGA_LED_R, FPGA_LED_G, FPGA_LED_B,
    output wire PS_CE_N, PS_CLK, inout wire [7:0] PS_DQ, inout wire PS_DQS,
    output wire AUD_BCLK, AUD_DIN, AUD_MCLK, AUD_RESET, AUD_WCLK,
    output wire IR_LED, output wire SDIO_LS, output wire ADC_SEL,
    inout wire POWER_DOWN_IO, inout wire LINK_CLK, output wire LINK_OUT,
    output wire LINK_CLK_DIR_LV, LINK_SD_DIR_LV,
    inout wire SCL, SDA, output wire CHG_EN_FPGA,
    inout wire usb_dxp_io, usb_dxn_io, usb_term_dp_io, usb_term_dn_io,
    output wire usb_pullup_en_o
);
    reg [15:0] boot_count=0;
    wire reset=boot_count<CLOCK_HZ/1000;
    always @(posedge CLK_FPGA) if (reset) boot_count<=boot_count+1'b1;
    reg [31:0] heartbeat=0;
    always @(posedge CLK_FPGA) if (reset) heartbeat<=0; else heartbeat<=heartbeat+1'b1;
    reg [8:0] buttons_meta=0, buttons=0;
    reg [2:0] straps_meta=0, straps=0;
    reg [1:0] power_sync=3, present_sync=0;
    always @(posedge CLK_FPGA) begin
        buttons_meta<={~BTN_MENU,BTN_DPAD_DOWN,BTN_DPAD_LEFT,BTN_DPAD_RIGHT,BTN_DPAD_UP,BTN_A,BTN_B,BTN_SEL,BTN_START};
        buttons<=buttons_meta;
        straps_meta<={DISPLAY_ID,VERSION_DET2,VERSION_DET}; straps<=straps_meta;
        power_sync<={power_sync[0],POWER_ON_FPGA};
        present_sync<={present_sync[0],CART_DET};
    end
    wire [7:0] menu_sequence;
    a2_menu_button #(.CLOCK_HZ(CLOCK_HZ)) menu_button (
        .clk(CLK_FPGA),.reset(reset),.pressed(buttons[8]),.press_sequence(menu_sequence)
    );
    wire switch_on=!power_sync[1];
    assign ESP32_EN=1'b1;
    assign ESP32_IO0=1'b1;
    assign ESP32_MCU_D4=1'b1; // USB-UART is not implemented in this controller.
    assign usb_pullup_en_o=1'b0;
    assign usb_dxp_io=1'bz;
    assign usb_dxn_io=1'bz;
    assign usb_term_dp_io=1'bz;
    assign usb_term_dn_io=1'bz;
    assign PS_CE_N=1'b1;
    assign PS_CLK=1'b0;
    assign PS_DQ=8'bz;
    assign PS_DQS=1'bz;
    assign CHG_EN_FPGA=1'b0;
    assign IR_LED=1'b0;
    assign SDIO_LS=1'b1;
    assign ADC_SEL=1'b0;
    assign POWER_DOWN_IO=1'bz;
    assign LINK_CLK=1'bz;
    assign LINK_OUT=1'b1;
    assign LINK_CLK_DIR_LV=1'b0;
    assign LINK_SD_DIR_LV=1'b0;
    assign FPGA_LED_EN=switch_on;
    assign FPGA_LED_R=!(|buttons);
    assign FPGA_LED_G=heartbeat[24];
    assign FPGA_LED_B=1'b1;

    wire audio_ready,audio_arm;
    wire [7:0] audio_flags,audio_error,audio_page,audio_register,audio_last_read;
    wire [71:0] audio_readback;
    wire [15:0] audio_din_ones,audio_frame_count;
    a2_audio #(.CLOCK_HZ(CLOCK_HZ)) audio (
        .clk(CLK_FPGA),.reset(reset||!switch_on),.arm(audio_arm),.esp_data(I2S_DOUT),
        .esp_bclk(I2S_BCLK),.esp_ws(I2S_WS),.codec_mclk(AUD_MCLK),
        .codec_bclk(AUD_BCLK),.codec_ws(AUD_WCLK),.codec_data(AUD_DIN),.codec_reset_n(AUD_RESET),
        .scl(SCL),.sda(SDA),.ready(audio_ready),.flags(audio_flags),.error_code(audio_error),
        .last_page(audio_page),.last_register(audio_register),.last_read(audio_last_read),
        .readback(audio_readback),.din_ones(audio_din_ones),.frame_count(audio_frame_count)
    );

    wire lcd_done;
    wire init_cs_n,init_sck,init_sda;
    a2_lcd_init #(.CLOCK_HZ(CLOCK_HZ)) lcd_init (
        .clk(CLK_FPGA),.reset(reset||!switch_on),.lcd_reset(LCD_RESET),
        .cs_n(init_cs_n),.sck(init_sck),.sda(init_sda),.done(lcd_done)
    );
    wire probe_cs_n,probe_sck,probe_sda,probe_oe,probe_done,probe_plausible;
    wire lcd_te_level;
    wire [15:0] lcd_te_edges;
    a2_lcd_te lcd_te_monitor (
        .clk(CLK_FPGA),.reset(reset||!switch_on||!lcd_done),.te_async(LCD_TE),
        .level(lcd_te_level),.edge_count(lcd_te_edges)
    );
    wire [47:0]lcd_readback;
    a2_lcd_readback lcd_probe (
        .clk(CLK_FPGA),.reset(reset||!switch_on),.start(lcd_done),.sda_in(LCD_SPI_SDA),
        .cs_n(probe_cs_n),.sck(probe_sck),.sda_out(probe_sda),.sda_oe(probe_oe),
        .done(probe_done),.plausible(probe_plausible),
        .power_mode(lcd_readback[47:40]),.orientation(lcd_readback[39:32]),.pixel_format(lcd_readback[31:24]),
        .id1(lcd_readback[23:16]),.id2(lcd_readback[15:8]),.id3(lcd_readback[7:0])
    );
    assign LCD_SPI_CSX=lcd_done ? probe_cs_n : init_cs_n;
    assign LCD_SPI_SCLK=lcd_done ? probe_sck : init_sck;
    assign LCD_SPI_SDA=!lcd_done ? init_sda : probe_oe ? probe_sda : 1'bz;
    wire [14:0] pixel_address;
    wire [3:0] pixel_data;
    wire frame_ready;
    a2_spi_video video (
        .clk(CLK_FPGA),.reset_n(~reset),.spi_sck(QSPI_CLK),.spi_mosi(QSPI_MOSI),.spi_cs_n(QSPI_CS),
        .pixel_address(pixel_address),.pixel_data(pixel_data),.frame_ready(frame_ready)
    );
    a2_display display (
        .clk(CLK_FPGA),.reset(reset),.enabled(lcd_done&&switch_on),
        .frame_ready(frame_ready),.pixel_data(pixel_data),.pixel_address(pixel_address),
        .dotclk(LCD_DOTCLK),.hsync(LCD_HSYNC),.vsync(LCD_VSYNC),
        .data_enable(LCD_ENABLE),.data(LCD_DB),.scan_x(),.scan_y()
    );
    assign LCD_PWM=lcd_done&&switch_on;
    wire read_start, write_start, read_done, read_failed, cart_ready;
    wire [15:0] read_address;
    wire [7:0] read_value;
    wire [7:0] write_value, cart_data_out;
    wire write_is_flash, write_is_burst;
    wire cart_reset_low;
    a2_cartridge #(.CLOCK_HZ(CLOCK_HZ),.CART_MODE(CART_MODE),.FLASH_WRITES(FLASH_WRITES)) cartridge (
        .clk(CLK_FPGA),.reset(reset),.switch_on(switch_on),.present(CART_DET),
        .legacy_strap(VERSION_DET),.ready(cart_ready),
        .read_start(read_start),.read_address(read_address),.read_done(read_done),
        .write_start(write_start),.write_value(write_value),.data_out(cart_data_out),
        .write_is_flash(write_is_flash),.write_is_burst(write_is_burst),
        .read_failed(read_failed),.read_value(read_value),.data_in(CART_D),
        .address(CART_A),.power_enable(CART_PWR_EN),.control_enable(CART_CTRL_OE),
        .reset_low(cart_reset_low),.cs_n(CART_CS),.rd_n(CART_RD),
        .wr_n(CART_WR),.data_input(CART_DATA_DIR_E)
    );
    assign CART_D=CART_DATA_DIR_E ? 8'bz : cart_data_out;
    assign CART_RST=cart_reset_low ? 1'b0 : 1'bz;
    assign CART_CLK=1'b1;
    wire [7:0] uart_rx, uart_tx;
    wire uart_rx_valid, uart_tx_start, uart_tx_busy;
    a2_uart #(.CLOCK_HZ(CLOCK_HZ),.BAUD(UART_BAUD)) uart (
        .clk(CLK_FPGA),.reset(reset),.rx(ESP32_MCU_D12),
        .rx_data(uart_rx),.rx_valid(uart_rx_valid),
        .tx_data(uart_tx),.tx_start(uart_tx_start),.tx(ESP32_MCU_D11),.tx_busy(uart_tx_busy)
    );
    wire req_valid, response_valid, response_ack;
    wire [7:0] req_length;
    wire [295:0] req_data;
    wire [287:0] response_data;
    wire [7:0] response_length;
    a2_packet packet (
        .clk(CLK_FPGA),.reset(reset),.rx_data(uart_rx),.rx_valid(uart_rx_valid),
        .req_valid(req_valid),.req_length(req_length),.req_data(req_data),
        .response_valid(response_valid),.response_data(response_data),.response_ack(response_ack),
        .response_length(response_length),
        .tx_data(uart_tx),.tx_start(uart_tx_start),.tx_busy(uart_tx_busy)
    );
    a2_control #(.FLASH_WRITES(FLASH_WRITES)) control (
        .clk(CLK_FPGA),.reset(reset),.req_valid(req_valid),.req_length(req_length),.req_data(req_data),
        .response_valid(response_valid),.response_data(response_data),.response_ack(response_ack),
        .response_length(response_length),
        .buttons(buttons),.menu_sequence(menu_sequence),
        .flags({3'd0,frame_ready,switch_on,lcd_done,cart_ready,present_sync[1]}),.straps(straps),
        .lcd_readback(lcd_readback),.lcd_readback_flags({probe_plausible,probe_done}),
        .lcd_te_level(lcd_te_level),.lcd_te_edges(lcd_te_edges),
        .audio_ready(audio_ready),.audio_arm(audio_arm),.audio_flags(audio_flags),.audio_error(audio_error),
        .audio_page(audio_page),.audio_register(audio_register),.audio_last_read(audio_last_read),
        .audio_readback(audio_readback),.audio_din_ones(audio_din_ones),.audio_frame_count(audio_frame_count),
        .cart_ready(cart_ready),.cart_start(read_start),.cart_address(read_address),
        .cart_write_start(write_start),.cart_write_value(write_value),
        .cart_write_is_flash(write_is_flash),.cart_write_is_burst(write_is_burst),
        .cart_done(read_done),.cart_failed(read_failed),.cart_value(read_value)
    );
endmodule
