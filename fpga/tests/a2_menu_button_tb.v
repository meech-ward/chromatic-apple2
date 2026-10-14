// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_menu_button_tb;
    reg clk=0;
    always #5 clk=~clk;
    reg reset=1,pressed=0;
    wire [7:0] press_sequence;
    // 1 kHz makes the 10 ms threshold exactly ten clock edges.
    a2_menu_button #(.CLOCK_HZ(1000)) dut(.*);

    task ticks(input integer n);
        repeat(n) begin @(posedge clk);#1;end
    endtask
    task level(input value);
        @(negedge clk);pressed=value;
    endtask
    task expect_sequence(input [7:0] expected);
        if(press_sequence!==expected)
            $fatal(1,"MENU sequence expected %0d, got %0d",expected,press_sequence);
    endtask
    task complete_press;
        begin level(0);ticks(10);level(1);ticks(10);end
    endtask

    integer i;
    initial begin
        ticks(3);expect_sequence(0);
        @(negedge clk);reset=0;
        ticks(10);expect_sequence(0);

        // A short pulse cannot become a press, even after the input is released.
        level(1);ticks(9);expect_sequence(0);
        level(0);ticks(12);expect_sequence(0);

        // Contact bounce restarts the timer; the tenth uninterrupted edge wins.
        level(1);ticks(4);
        level(0);ticks(2);
        level(1);ticks(9);expect_sequence(0);
        ticks(1);expect_sequence(1);
        ticks(100);expect_sequence(1); // Held MENU does not repeat.

        // A release also has to debounce before another press can count.
        level(0);ticks(9);
        level(1);ticks(20);expect_sequence(1);
        complete_press;expect_sequence(2);

        // Many presses survive arbitrarily delayed MCU reads, modulo 256.
        for(i=0;i<253;i=i+1) complete_press;
        expect_sequence(255);
        complete_press;expect_sequence(0);
        complete_press;expect_sequence(1);

        // Reset clears both the sequence and a partially debounced transition.
        level(0);ticks(10);
        level(1);ticks(5);
        @(negedge clk);reset=1;
        ticks(2);expect_sequence(0);
        @(negedge clk);reset=0;
        ticks(9);expect_sequence(0);
        ticks(1);expect_sequence(1);
        ticks(30);expect_sequence(1);

        $display("PASS: retained MENU debounce, short pulses, bounce, hold/release, rollover and reset");
        $finish;
    end
    initial begin #1000000;$fatal(1,"MENU test timeout");end
endmodule
