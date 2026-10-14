// SPDX-License-Identifier: MIT
// Original coherent I2S clock/relay. ESP32 is slave TX; codec is slave RX.
module a2_audio #(
    parameter CLOCK_HZ=33554432,
    parameter I2C_HALF_TICKS=CLOCK_HZ/200000,
    parameter STARTUP_TICKS=CLOCK_HZ/1000,
    parameter SETTLE_TICKS=CLOCK_HZ/2,
    parameter I2C_TIMEOUT_TICKS=CLOCK_HZ/50
) (
    input wire clk, input wire reset, input wire arm, input wire esp_data,
    output wire esp_bclk, output wire esp_ws,
    output wire codec_mclk, output wire codec_bclk, output wire codec_ws,
    output wire codec_data, output wire codec_reset_n,
    inout wire scl, inout wire sda,
    output wire ready, output wire [7:0] flags,error_code,last_page,last_register,last_read,
    output wire [71:0] readback,
    output reg [15:0] din_ones=0, output reg [15:0] frame_count=0
);
    reg [9:0] clock_count=0;
    (* async_reg = "true" *) reg [1:0] data_sync=0;
    reg streaming=0;
    wire init_busy;
    assign codec_mclk=clock_count[1];
    assign codec_bclk=clock_count[4];
    assign codec_ws=clock_count[9];
    assign esp_bclk=codec_bclk;
    assign esp_ws=codec_ws;
    assign codec_data=streaming && ready ? data_sync[1] : 1'b0;
    assign flags={3'd0,(error_code!=0),init_busy,codec_reset_n,arm,ready};
    a2_codec_init #(.CLOCK_HZ(CLOCK_HZ),.I2C_HALF_TICKS(I2C_HALF_TICKS),
        .STARTUP_TICKS(STARTUP_TICKS),.SETTLE_TICKS(SETTLE_TICKS),
        .I2C_TIMEOUT_TICKS(I2C_TIMEOUT_TICKS)) codec (
        .clk(clk),.reset(reset),.scl(scl),.sda(sda),.codec_reset_n(codec_reset_n),
        .ready(ready),.busy(init_busy),.error_code(error_code),
        .last_page(last_page),.last_register(last_register),.last_read(last_read),.readback(readback)
    );
    always @(posedge clk) begin
        if (reset) begin
            clock_count<=0; data_sync<=0; streaming<=0; din_ones<=0; frame_count<=0;
        end else begin
            clock_count<=clock_count+1'b1;
            data_sync<={data_sync[0],esp_data};
            if (!ready) streaming<=0;
            // WS changes at count0. Count32 launches the next left MSB after
            // the Philips one-bit delay; update gating between complete words.
            else if (clock_count==31) begin
                streaming<=arm;
                if (streaming) frame_count<=frame_count+1'b1;
            end
            if (streaming && ready && clock_count[4:0]==15 && data_sync[1])
                din_ones<=din_ones+1'b1;
        end
    end
endmodule
