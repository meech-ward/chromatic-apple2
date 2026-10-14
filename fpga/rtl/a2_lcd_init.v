// SPDX-License-Identifier: MIT
// Datasheet-derived, bounded initialization; analog drive registers retain defaults.
// ST7785M preliminary v0.0: sections 9.1.12, 9.1.27, 9.1.28, 9.1.32, 9.2.1, 9.2.2, 9.2.29.
module a2_lcd_init #(parameter CLOCK_HZ=33554432, parameter SPI_HALF=64) (
    input wire clk, input wire reset,
    output reg lcd_reset=0, output reg cs_n=1, output reg sck=0,
    output reg sda=0, output reg done=0
);
    reg [2:0] state=0;
    reg [31:0] wait_left=0;
    reg [15:0] spi_left=0;
    reg [4:0] index=0;
    reg [3:0] bits_left=0;
    reg [8:0] shift=0;
    function [8:0] command_word;
        input [4:0] n;
        begin
            case(n)
                0: command_word=9'h011; // sleep out
                1: command_word=9'h0b0; // RGB RAM access/display selection
                2: command_word=9'h111;
                3: command_word=9'h1f4;
                4: command_word=9'h0b1; // board RGB interface: falling DOTCLK sample
                5: command_word=9'h1c2;
                6: command_word=9'h108;
                7: command_word=9'h160;
                8: command_word=9'h0e4; // 144 gate lines, board gate start88
                9: command_word=9'h111;
                10: command_word=9'h10b;
                11: command_word=9'h110;
                12: command_word=9'h03a; // RGB666
                13: command_word=9'h166;
                14: command_word=9'h036; // normal scan/order
                15: command_word=9'h100;
                16: command_word=9'h013; // normal mode
                17: command_word=9'h021; // board panel requires display inversion
                18: command_word=9'h029; // display on
                19: command_word=9'h035; // TE output on, vertical blanking only
                default: command_word=9'h100;
            endcase
        end
    endfunction
    always @(posedge clk) begin
        if (reset) begin
            state<=0; wait_left<=CLOCK_HZ/100; index<=0;
            cs_n<=1; sck<=0; sda<=0; lcd_reset<=0; done<=0;
            spi_left<=0; bits_left<=0; shift<=0;
        end else case(state)
            0: if (wait_left>1) wait_left<=wait_left-1'b1;
               else begin lcd_reset<=1; wait_left<=CLOCK_HZ*12/100; state<=1; end
            1: if (wait_left>1) wait_left<=wait_left-1'b1;
               else state<=2;
            2: begin
                   shift<=command_word(index); sda<=command_word(index)>>8;
                   bits_left<=9; cs_n<=0; sck<=0; spi_left<=SPI_HALF; state<=3;
               end
            3: if (spi_left>1) spi_left<=spi_left-1'b1;
               else begin sck<=1; spi_left<=SPI_HALF; state<=4; end
            4: if (spi_left>1) spi_left<=spi_left-1'b1;
               else begin
                   sck<=0;
                   if (bits_left==1) begin
                       cs_n<=1;
                       if (index==20) begin wait_left<=CLOCK_HZ/50; state<=5; end
                       else begin
                           index<=index+1'b1;
                           wait_left<=index==0 ? CLOCK_HZ*12/100 : SPI_HALF;
                           state<=1;
                       end
                   end else begin
                       shift<={shift[7:0],1'b0}; sda<=shift[7];
                       bits_left<=bits_left-1'b1; spi_left<=SPI_HALF; state<=3;
                   end
               end
            5: if (wait_left>1) wait_left<=wait_left-1'b1;
               else begin done<=1; state<=6; end
            6: ;
            default: state<=0;
        endcase
    end
endmodule
