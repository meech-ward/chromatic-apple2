// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_codec_i2c_tb;
    reg clk=0;always #5 clk=~clk;
    reg reset=1,start=0,reading=0,nack_all=0,corrupt_read=0,fail_power=0,stretch=0;
    reg[7:0]register_address=0,write_data=0;
    wire busy,done;
    wire[1:0]error;
    wire[7:0]read_data;
    tri1 scl,sda;
    reg stuck_sda=0;
    assign sda=stuck_sda ? 1'b0 : 1'bz;
    a2_codec_i2c #(.HALF_TICKS(8),.TIMEOUT_TICKS(5000)) dut(.*);
    a2_codec_model model(.reset_n(!reset),.nack_all(nack_all),.corrupt_read(corrupt_read),
        .fail_power(fail_power),.stretch(stretch),.scl(scl),.sda(sda));
    task clocks(input integer count);repeat(count)begin @(posedge clk);#1;end endtask
    task request(input read_request,input[7:0]address,value);
        begin
            @(negedge clk);reading=read_request;register_address=address;write_data=value;start=1;
            @(negedge clk);start=0;
        end
    endtask
    task completion(input[1:0]expected_error,input[7:0]expected_read);
        integer count;
        begin
            count=0;
            while(!done&&count<6000)begin clocks(1);count=count+1;end
            if(!done||busy||error!==expected_error||(reading&&error==0&&read_data!==expected_read))
                $fatal(1,"I2C completion mismatch error%0d read%h",error,read_data);
            clocks(2);
            if(done)$fatal(1,"I2C completion is not a one-cycle pulse");
        end
    endtask
    initial begin
        clocks(3);@(negedge clk);reset=0;
        request(0,8'h0b,8'h81);completion(0,0);
        request(1,8'h0b,0);completion(0,8'h81);
        if(model.reads!=1||model.writes!=1||model.starts!=3)$fatal(1,"Repeated START/register transfer incorrect");
        request(0,8'h0c,8'h82);
        wait(dut.state==dut.TX_LOW);@(negedge clk);stretch=1;clocks(40);
        if(!busy||done)$fatal(1,"Clock stretch was not honored");
        @(negedge clk);stretch=0;completion(0,0);
        request(1,8'h0c,0);completion(0,8'h82);
        nack_all=1;request(0,8'h0b,8'h22);completion(1,0);
        if(scl!==1||sda!==1)$fatal(1,"NACK did not release bus with STOP");
        nack_all=0;stretch=1;request(0,8'h0b,8'h22);completion(2,0);
        stretch=0;clocks(3);
        stuck_sda=1;request(0,8'h0b,8'h22);completion(2,0);
        stuck_sda=0;clocks(3);
        request(0,8'h0b,8'h22);wait(busy);@(negedge clk);reset=1;clocks(2);
        if(busy||done||scl!==1||sda!==1)$fatal(1,"Reset left I2C bus driven");
        $display("PASS: I2C register write/read, repeated START, ACK/NACK, stretching, timeout and reset");$finish;
    end
    initial begin #1000000;$fatal(1,"Timeout");end
endmodule
