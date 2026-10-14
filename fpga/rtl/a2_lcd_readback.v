// SPDX-License-Identifier: MIT
// Original bounded, read-only ST7785M serial Interface I diagnostic.
// Standard8-bit register reads need no dummy clock (datasheet section8.3.4).
module a2_lcd_readback #(parameter SPI_HALF=64) (
    input wire clk, input wire reset, input wire start,
    input wire sda_in,
    output reg cs_n=1, output reg sck=0,
    output reg sda_out=0, output reg sda_oe=0,
    output reg done=0, output wire plausible,
    output reg [7:0] power_mode=0,
    output reg [7:0] orientation=0,
    output reg [7:0] pixel_format=0,
    output reg [7:0] id1=0,
    output reg [7:0] id2=0,
    output reg [7:0] id3=0
);
    localparam WAIT=0, BEGIN_WORD=1, TX_LOW=2, TX_HIGH=3,
               RX_LOW=4,RX_HIGH=5,GAP=6,FINISH=7;
    reg [2:0]state=WAIT,index=0;
    reg [15:0]timer=0;
    reg [8:0]tx_shift=0;
    reg [7:0]rx_shift=0;
    reg [3:0]bits_left=0;
    reg [1:0]sda_sync=0;
    assign plausible=done && {id1,id2,id3}!=24'd0 && {id1,id2,id3}!=24'hffffff &&
                      power_mode[4:2]==3'b111 && orientation==0 && pixel_format==8'h66;
    function [7:0]register_command;
        input [2:0]which;
        begin
            case(which)
                0:register_command=8'h0a;
                1:register_command=8'h0b;
                2:register_command=8'h0c;
                3:register_command=8'hda;
                4:register_command=8'hdb;
                default:register_command=8'hdc;
            endcase
        end
    endfunction
    always @(posedge clk) begin
        if(reset)begin
            state<=WAIT;index<=0;timer<=0;tx_shift<=0;rx_shift<=0;bits_left<=0;
            sda_sync<=0;cs_n<=1;sck<=0;sda_out<=0;sda_oe<=0;done<=0;
            power_mode<=0;orientation<=0;pixel_format<=0;id1<=0;id2<=0;id3<=0;
        end else begin
            sda_sync<={sda_sync[0],sda_in};
            case(state)
                WAIT:if(start)state<=BEGIN_WORD;
                BEGIN_WORD:begin
                    cs_n<=0;sck<=0;sda_oe<=1;sda_out<=0;
                    tx_shift<={1'b0,register_command(index)};bits_left<=9;
                    timer<=SPI_HALF;state<=TX_LOW;
                end
                TX_LOW:if(timer>1)timer<=timer-1'b1;
                       else begin sck<=1;timer<=SPI_HALF;state<=TX_HIGH;end
                TX_HIGH:begin
                    // Release one core clock before the final falling edge.
                    if(bits_left==1 && timer==2)sda_oe<=0;
                    if(timer>1)timer<=timer-1'b1;
                    else begin
                        sck<=0;timer<=SPI_HALF;
                        if(bits_left==1)begin sda_oe<=0;bits_left<=8;rx_shift<=0;state<=RX_LOW;end
                        else begin
                            tx_shift<={tx_shift[7:0],1'b0};sda_out<=tx_shift[7];
                            bits_left<=bits_left-1'b1;state<=TX_LOW;
                        end
                    end
                end
                RX_LOW:if(timer>1)timer<=timer-1'b1;
                       else begin sck<=1;rx_shift<={rx_shift[6:0],sda_sync[1]};timer<=SPI_HALF;state<=RX_HIGH;end
                RX_HIGH:if(timer>1)timer<=timer-1'b1;
                        else begin
                            sck<=0;timer<=SPI_HALF;
                            if(bits_left==1)begin
                                cs_n<=1;
                                case(index)
                                    0:power_mode<=rx_shift;
                                    1:orientation<=rx_shift;
                                    2:pixel_format<=rx_shift;
                                    3:id1<=rx_shift;
                                    4:id2<=rx_shift;
                                    5:id3<=rx_shift;
                                endcase
                                if(index==5)begin done<=1;state<=FINISH;end
                                else begin index<=index+1'b1;state<=GAP;end
                            end else begin bits_left<=bits_left-1'b1;state<=RX_LOW;end
                        end
                GAP:if(timer>1)timer<=timer-1'b1;else state<=BEGIN_WORD;
                FINISH:;
                default:begin cs_n<=1;sck<=0;sda_oe<=0;state<=WAIT;end
            endcase
        end
    end
endmodule
