// SPDX-License-Identifier: MIT
// Original bounded single-register I2C master, fixed to the audio codec at0x18.
// Open-drain pins, synchronized inputs, clock stretching and transaction timeout.
module a2_codec_i2c #(
    parameter HALF_TICKS=168, parameter TIMEOUT_TICKS=671089
) (
    input wire clk, input wire reset, input wire start, input wire reading,
    input wire [7:0] register_address, input wire [7:0] write_data,
    output reg busy=0, output reg done=0, output reg [1:0] error=0,
    output reg [7:0] read_data=0,
    inout wire scl, inout wire sda
);
    localparam IDLE=0, FREE=1, START_HOLD=2, TX_SETUP=3, TX_LOW=4, TX_HIGH=5,
               ACK_SETUP=6, ACK_LOW=7, ACK_HIGH=8, RESTART_LOW=9, RESTART_HIGH=10,
               RX_SETUP=11, RX_LOW=12, RX_HIGH=13, NACK_LOW=14, NACK_HIGH=15,
               STOP_SETUP=16, STOP_LOW=17, STOP_HIGH=18, STOP_FREE=19;
    reg [4:0] state=IDLE;
    reg scl_low=0,sda_low=0;
    assign scl=scl_low ? 1'b0 : 1'bz;
    assign sda=sda_low ? 1'b0 : 1'bz;
    (* async_reg = "true" *) reg [1:0] scl_sync=3,sda_sync=3;
    reg [31:0] timer=0,watchdog=0;
    reg [7:0] tx=0,rx=0,saved_register=0,saved_value=0;
    reg saved_read=0;
    reg [2:0] bit_index=7;
    reg [1:0] stage=0;
    always @(posedge clk) begin
        scl_sync<={scl_sync[0],scl};
        sda_sync<={sda_sync[0],sda};
        done<=0;
        if (reset) begin
            state<=IDLE; busy<=0; done<=0; error<=0; read_data<=0;
            scl_low<=0; sda_low<=0; timer<=0; watchdog<=0;
            tx<=0; rx<=0; saved_register<=0; saved_value<=0; saved_read<=0;
            bit_index<=7; stage<=0; scl_sync<=3; sda_sync<=3;
        end else if (busy && watchdog==0) begin
            state<=IDLE; busy<=0; done<=1; error<=2;
            scl_low<=0; sda_low<=0;
        end else begin
            if (busy) watchdog<=watchdog-1'b1;
            case (state)
                IDLE: if (start) begin
                    busy<=1; error<=0; read_data<=0;
                    saved_register<=register_address; saved_value<=write_data; saved_read<=reading;
                    watchdog<=TIMEOUT_TICKS; timer<=HALF_TICKS; state<=FREE;
                    scl_low<=0; sda_low<=0; stage<=0; tx<=8'h30; bit_index<=7;
                end
                FREE: if (!scl_sync[1] || !sda_sync[1]) timer<=HALF_TICKS;
                      else if (timer>1) timer<=timer-1'b1;
                      else begin sda_low<=1; timer<=HALF_TICKS; state<=START_HOLD; end
                START_HOLD: if (timer>1) timer<=timer-1'b1;
                            else begin scl_low<=1; state<=TX_SETUP; end
                TX_SETUP: begin
                    sda_low<=!tx[bit_index]; timer<=HALF_TICKS; state<=TX_LOW;
                end
                TX_LOW: if (timer>1) timer<=timer-1'b1;
                        else begin scl_low<=0; timer<=HALF_TICKS; state<=TX_HIGH; end
                TX_HIGH: if (scl_sync[1]) begin
                    if (timer>1) timer<=timer-1'b1;
                    else begin
                        scl_low<=1;
                        if (bit_index==0) state<=ACK_SETUP;
                        else begin bit_index<=bit_index-1'b1; state<=TX_SETUP; end
                    end
                end
                ACK_SETUP: begin sda_low<=0; timer<=HALF_TICKS; state<=ACK_LOW; end
                ACK_LOW: if (timer>1) timer<=timer-1'b1;
                         else begin scl_low<=0; timer<=HALF_TICKS; state<=ACK_HIGH; end
                ACK_HIGH: if (scl_sync[1]) begin
                    if (timer>1) timer<=timer-1'b1;
                    else begin
                        scl_low<=1; bit_index<=7;
                        if (sda_sync[1]) begin error<=1; state<=STOP_SETUP; end
                        else case (stage)
                            0: begin tx<=saved_register; stage<=1; state<=TX_SETUP; end
                            1: if (saved_read) begin
                                   sda_low<=0; timer<=HALF_TICKS; state<=RESTART_LOW;
                               end else begin tx<=saved_value; stage<=2; state<=TX_SETUP; end
                            2: state<=STOP_SETUP;
                            3: state<=RX_SETUP;
                        endcase
                    end
                end
                RESTART_LOW: if (timer>1) timer<=timer-1'b1;
                             else begin scl_low<=0; timer<=HALF_TICKS; state<=RESTART_HIGH; end
                RESTART_HIGH: if (scl_sync[1]) begin
                    if (timer>1) timer<=timer-1'b1;
                    else begin
                        sda_low<=1; tx<=8'h31; stage<=3; timer<=HALF_TICKS; state<=START_HOLD;
                    end
                end
                RX_SETUP: begin sda_low<=0; timer<=HALF_TICKS; state<=RX_LOW; end
                RX_LOW: if (timer>1) timer<=timer-1'b1;
                        else begin scl_low<=0; timer<=HALF_TICKS; state<=RX_HIGH; end
                RX_HIGH: if (scl_sync[1]) begin
                    if (timer>1) timer<=timer-1'b1;
                    else begin
                        scl_low<=1; rx[bit_index]<=sda_sync[1];
                        if (bit_index==0) begin
                            read_data<={rx[7:1],sda_sync[1]};
                            timer<=HALF_TICKS; state<=NACK_LOW;
                        end else begin bit_index<=bit_index-1'b1; state<=RX_SETUP; end
                    end
                end
                NACK_LOW: if (timer>1) timer<=timer-1'b1;
                          else begin scl_low<=0; timer<=HALF_TICKS; state<=NACK_HIGH; end
                NACK_HIGH: if (scl_sync[1]) begin
                    if (timer>1) timer<=timer-1'b1;
                    else begin scl_low<=1; state<=STOP_SETUP; end
                end
                STOP_SETUP: begin sda_low<=1; timer<=HALF_TICKS; state<=STOP_LOW; end
                STOP_LOW: if (timer>1) timer<=timer-1'b1;
                          else begin scl_low<=0; timer<=HALF_TICKS; state<=STOP_HIGH; end
                STOP_HIGH: if (scl_sync[1]) begin
                    if (timer>1) timer<=timer-1'b1;
                    else begin sda_low<=0; timer<=HALF_TICKS; state<=STOP_FREE; end
                end
                STOP_FREE: if (timer>1) timer<=timer-1'b1;
                           else begin done<=1; busy<=0; state<=IDLE; end
                default: begin state<=IDLE; busy<=0; scl_low<=0; sda_low<=0; error<=2; done<=1; end
            endcase
        end
    end
endmodule
