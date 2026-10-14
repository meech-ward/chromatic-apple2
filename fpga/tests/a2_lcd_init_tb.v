// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_lcd_init_tb;
    reg clk=0;always #5 clk=~clk;
    reg reset=1;
    wire lcd_reset,cs_n,sck,sda,done;
    a2_lcd_init #(.CLOCK_HZ(1000),.SPI_HALF(2)) dut(.*);
    integer cycle=0,release_cycle=0,last_word_cycle=0,words=0,bits=0;
    reg[8:0]word=0;
    wire [188:0] expected_words={9'h011,9'h0b0,9'h111,9'h1f4,
        9'h0b1,9'h1c2,9'h108,9'h160,9'h0e4,9'h111,9'h10b,9'h110,
        9'h03a,9'h166,9'h036,9'h100,9'h013,9'h021,9'h029,9'h035,9'h100};
    always @(posedge clk)cycle=cycle+1;
    always @(posedge lcd_reset)begin
        if(cycle<10)$fatal(1,"Hardware reset pulse too short");
        release_cycle=cycle;
    end
    always @(negedge cs_n)begin
        bits=0;word=0;
        if(!lcd_reset)$fatal(1,"Serial transfer while display reset asserted");
        if(words==0&&cycle-release_cycle<120)$fatal(1,"Insufficient reset recovery");
        if(words==1&&cycle-last_word_cycle<120)$fatal(1,"Insufficient sleep-out stabilization");
    end
    always @(posedge sck)begin
        if(cs_n||done)$fatal(1,"SPI clock outside an active initialization transfer");
        word={word[7:0],sda};bits=bits+1;
    end
    always @(posedge cs_n)begin
        if(!reset&&bits!=0)begin
            if(bits!=9)$fatal(1,"Display serial word is not 9 bits");
            if(words>=21||word!==expected_words[188-words*9 -: 9])
                $fatal(1,"Initialization word %0d is incorrect: %h",words,word);
            words=words+1;last_word_cycle=cycle;
            if(words==1&&word!=9'h011)$fatal(1,"First command is not sleep-out");
        end
    end
    initial begin
        repeat(3)@(posedge clk);@(negedge clk);reset=0;
        wait(done);#1;
        if(words!=21||!cs_n||sck||!lcd_reset||cycle-last_word_cycle<20)
            $fatal(1,"Initialization completion or final stabilization invalid");
        repeat(50)@(posedge clk);
        $display("PASS: bounded LCD reset/recovery, sleep-out delay, TEON command sequence and quiet completion");$finish;
    end
    initial begin #50000;$fatal(1,"Timeout");end
endmodule
