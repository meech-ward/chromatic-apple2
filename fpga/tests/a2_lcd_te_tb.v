// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_lcd_te_tb;
    reg clk=0; always #5 clk=~clk;
    reg reset=1,te_async=0;
    wire level;
    wire [15:0] edge_count;
    a2_lcd_te dut(.*);
    integer i;
    task clocks(input integer count);
        begin repeat(count) begin @(posedge clk); #1; end end
    endtask
    task pulse;
        begin
            @(negedge clk);te_async=1;clocks(5);
            @(negedge clk);te_async=0;clocks(5);
        end
    endtask
    initial begin
        clocks(3);
        if(level!==0||edge_count!==0)$fatal(1,"TE reset state is not quiet");
        @(negedge clk);reset=0;
        #2;te_async=1;
        clocks(1);
        if(level!==0||edge_count!==0)$fatal(1,"TE input bypassed first synchronizer");
        clocks(1);
        if(level!==1||edge_count!==0)$fatal(1,"TE second synchronizer/edge pipeline incorrect");
        clocks(1);
        if(edge_count!==1)$fatal(1,"TE rising edge was not counted");
        clocks(20);
        if(edge_count!==1)$fatal(1,"Steady high TE counted more than once");
        @(negedge clk);te_async=0;clocks(5);
        if(level!==0||edge_count!==1)$fatal(1,"TE falling edge counted");
        pulse();
        if(edge_count!==2)$fatal(1,"Second TE pulse missing");
        @(negedge clk);reset=1;te_async=1;clocks(3);
        if(level!==0||edge_count!==0)$fatal(1,"Reset failed to clear TE state");
        @(negedge clk);reset=0;clocks(5);
        if(level!==1||edge_count!==1)$fatal(1,"TE high after reset was not sampled");
        @(negedge clk);reset=1;te_async=0;clocks(3);
        @(negedge clk);reset=0;
        for(i=1;i<=65537;i=i+1)begin
            pulse();
            if(edge_count!==i[15:0])$fatal(1,"TE counter wrap mismatch at pulse %0d",i);
        end
        $display("PASS: TE synchronization, rising edges only, reset, steady level and 16-bit wrap");
        $finish;
    end
    initial begin #20000000;$fatal(1,"Timeout");end
endmodule
