// SPDX-License-Identifier: MIT
// Original TLV320DAC3100-family mono-speaker setup from TI SLAS671C.
// PLL bypass,32.768kHz,16-bit I2S; headphones off; -30dB DAC,-9dB analog,+6dB amp.
// Every writable setting is read back before final unmute. No device-ID claim.
module a2_codec_init #(
    parameter CLOCK_HZ=33554432,
    parameter I2C_HALF_TICKS=CLOCK_HZ/200000,
    parameter STARTUP_TICKS=CLOCK_HZ/1000,
    parameter SETTLE_TICKS=CLOCK_HZ/2,
    parameter I2C_TIMEOUT_TICKS=CLOCK_HZ/50
) (
    input wire clk, input wire reset,
    inout wire scl, inout wire sda,
    output reg codec_reset_n=0, output reg ready=0,
    output wire busy, output reg [7:0] error_code=0,
    output reg [7:0] last_page=0, output reg [7:0] last_register=0, output reg [7:0] last_read=0,
    output reg [71:0] readback=0
);
    localparam HOLD_RESET=0, WAIT_STARTUP=1, PAGE_START=2, PAGE_WAIT=3,
               WRITE_START=4, WRITE_WAIT=5, READ_START=6, READ_WAIT=7,
               WAIT_SETTLE=8, COMPLETE=9, FAILED=10;
    reg [3:0] state=HOLD_RESET;
    reg [7:0] step=0;
    reg [31:0] timer=STARTUP_TICKS;
    reg i2c_start=0,i2c_reading=0;
    reg [7:0] i2c_register=0,i2c_value=0;
    wire i2c_busy,i2c_done;
    wire [1:0] i2c_error;
    wire [7:0] i2c_read_data;
    assign busy=state!=COMPLETE && state!=FAILED;
    a2_codec_i2c #(.HALF_TICKS(I2C_HALF_TICKS),.TIMEOUT_TICKS(I2C_TIMEOUT_TICKS)) master (
        .clk(clk),.reset(reset||state==FAILED),.start(i2c_start),.reading(i2c_reading),
        .register_address(i2c_register),.write_data(i2c_value),
        .busy(i2c_busy),.done(i2c_done),.error(i2c_error),.read_data(i2c_read_data),
        .scl(scl),.sda(sda)
    );
    // page,register,value,verification-mask. Step18 is a read-only power check.
    function [31:0] setting;
        input [7:0] number;
        begin
            case (number)
                0: setting=32'h00010100; // software reset; self-clearing
                1: setting=32'h000400ff; // MCLK direct, codec PLL bypassed
                2: setting=32'h000b81ff; // NDAC1, powered
                3: setting=32'h000c82ff; // MDAC2, powered
                4: setting=32'h000d00ff; // DOSR128, high bits
                5: setting=32'h000e80ff;
                6: setting=32'h001b00ff; // I2S,16-bit, codec BCLK/WS inputs
                7: setting=32'h001c00ff; // no additional data offset
                8: setting=32'h003c01ff; // PRB_P1, resource class8
                9: setting=32'h00400cff; // both DAC channels muted
                10: setting=32'h0041c4ff; // left DAC -30dB
                11: setting=32'h011f06fe; // headphone drivers off, reserved bit2=1
                12: setting=32'h012006ff; // speaker off; reserved bits retained
                13: setting=32'h012340ff; // left DAC to left mixer only
                14: setting=32'h012692ff; // mixer to speaker, analog -9dB
                15: setting=32'h012a00fe; // speaker muted, gain6dB
                16: setting=32'h003f94ff; // left DAC on; left serial data; soft steps
                17: setting=32'h012086ff; // speaker on, still muted
                18: setting=32'h00259090; // left DAC+speaker power flags
                19: setting=32'h012a04fe; // unmute speaker after verification/settling
                20: setting=32'h004004ff; // left DAC unmuted, right remains muted
                default: setting=0;
            endcase
        end
    endfunction
    wire [31:0] current=setting(step);
    always @(posedge clk) begin
        i2c_start<=0;
        if (reset) begin
            state<=HOLD_RESET; step<=0; timer<=STARTUP_TICKS;
            codec_reset_n<=0; ready<=0; error_code<=0;
            last_page<=0; last_register<=0; last_read<=0; readback<=0;
            i2c_start<=0; i2c_reading<=0; i2c_register<=0; i2c_value<=0;
        end else case (state)
            HOLD_RESET: if (timer>1) timer<=timer-1'b1;
                        else begin codec_reset_n<=1; timer<=STARTUP_TICKS; state<=WAIT_STARTUP; end
            WAIT_STARTUP: if (timer>1) timer<=timer-1'b1;
                          else state<=PAGE_START;
            PAGE_START: if (!i2c_busy) begin
                i2c_register<=0; i2c_value<=current[31:24]; i2c_reading<=0; i2c_start<=1;
                last_page<=current[31:24]; state<=PAGE_WAIT;
            end
            PAGE_WAIT: if (i2c_done) begin
                if (i2c_error!=0) begin error_code<={6'd0,i2c_error}; state<=FAILED; codec_reset_n<=0; end
                else state<=step==18 ? READ_START : WRITE_START;
            end
            WRITE_START: if (!i2c_busy) begin
                i2c_register<=current[23:16]; i2c_value<=current[15:8];
                i2c_reading<=0; i2c_start<=1; last_register<=current[23:16]; state<=WRITE_WAIT;
            end
            WRITE_WAIT: if (i2c_done) begin
                if (i2c_error!=0) begin error_code<={6'd0,i2c_error}; state<=FAILED; codec_reset_n<=0; end
                else if (step==0) begin step<=1; timer<=STARTUP_TICKS; state<=WAIT_STARTUP; end
                else state<=READ_START;
            end
            READ_START: if (!i2c_busy) begin
                i2c_register<=current[23:16]; i2c_reading<=1; i2c_start<=1;
                last_register<=current[23:16]; state<=READ_WAIT;
            end
            READ_WAIT: if (i2c_done) begin
                last_read<=i2c_read_data;
                case ({current[31:24],current[23:16]})
                    16'h0004: readback[71:64]<=i2c_read_data;
                    16'h000b: readback[63:56]<=i2c_read_data;
                    16'h000c: readback[55:48]<=i2c_read_data;
                    16'h000e: readback[47:40]<=i2c_read_data;
                    16'h001b: readback[39:32]<=i2c_read_data;
                    16'h003f: readback[31:24]<=i2c_read_data;
                    16'h012a: readback[23:16]<=i2c_read_data;
                    16'h0120: readback[15:8]<=i2c_read_data;
                    16'h0025: readback[7:0]<=i2c_read_data;
                endcase
                if (i2c_error!=0) begin error_code<={6'd0,i2c_error}; state<=FAILED; codec_reset_n<=0; end
                else if ((i2c_read_data & current[7:0])!=(current[15:8] & current[7:0])) begin
                    error_code<=step==18 ? 4 : 3; state<=FAILED; codec_reset_n<=0;
                end else if (step==20) begin ready<=1; state<=COMPLETE; end
                else begin
                    step<=step+1'b1;
                    if (step==17) begin timer<=SETTLE_TICKS; state<=WAIT_SETTLE; end
                    else state<=PAGE_START;
                end
            end
            WAIT_SETTLE: if (timer>1) timer<=timer-1'b1;
                         else state<=PAGE_START;
            COMPLETE: ;
            FAILED: begin ready<=0; codec_reset_n<=0; end
            default: begin ready<=0; codec_reset_n<=0; error_code<=3; state<=FAILED; end
        endcase
    end
endmodule
