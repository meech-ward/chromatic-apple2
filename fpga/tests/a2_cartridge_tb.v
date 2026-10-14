// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_cartridge_tb;
    reg clk=0; always #5 clk=~clk;
    reg reset=1, switch_on=0, present=0, legacy_strap=0, read_start=0;
    reg [15:0] read_address=0;
    reg write_start=0;
    reg write_is_flash=0,write_is_burst=0;
    reg [7:0] write_value=0;
    wire [7:0] data_out;
    wire ready, read_done, read_failed, power_enable, control_enable, reset_low, cs_n,rd_n,wr_n,data_input;
    wire [15:0] address;
    wire [7:0] read_value;
    wire [7:0] data_in=address[7:0]^8'h55;
    a2_cartridge #(.CART_MODE(2),.POWER_TICKS(4),.RESET_TICKS(2),.SETTLE_TICKS(4),
        .SHUTDOWN_TICKS(3),.SETUP_TICKS(3),.READ_TICKS(5),.HOLD_TICKS(2)) dut(.*);
    wire disabled_power,disabled_control,disabled_ready,disabled_wr,disabled_input;
    a2_cartridge #(.CART_MODE(0),.FLASH_WRITES(1)) disabled_dut(
        .clk(clk),.reset(reset),.switch_on(switch_on),.present(present),.legacy_strap(legacy_strap),
        .read_start(1'b0),.read_address(16'h4055),.data_in(8'd0),
        .write_start(1'b1),.write_value(8'h98),
        .write_is_flash(1'b1),.write_is_burst(1'b1),
        .power_enable(disabled_power),.control_enable(disabled_control),.ready(disabled_ready),
        .wr_n(disabled_wr),.data_input(disabled_input)
    );
    reg direct_start=0,direct_flash=1,direct_burst=1;
    reg [15:0]direct_address=0;
    reg [7:0]direct_value=0;
    wire direct_ready,direct_done,direct_failed,direct_wr,direct_input;
    a2_cartridge #(.CART_MODE(2),.FLASH_WRITES(1),.POWER_TICKS(4),.RESET_TICKS(2),.SETTLE_TICKS(4),
        .SHUTDOWN_TICKS(3),.SETUP_TICKS(3),.WRITE_TICKS(4),.HOLD_TICKS(2)) burst_backend(
        .clk(clk),.reset(reset),.switch_on(switch_on),.present(present),.legacy_strap(legacy_strap),
        .read_start(1'b0),.read_address(direct_address),.data_in(8'd0),
        .write_start(direct_start),.write_value(direct_value),
        .write_is_flash(direct_flash),.write_is_burst(direct_burst),
        .ready(direct_ready),.read_done(direct_done),.read_failed(direct_failed),
        .wr_n(direct_wr),.data_input(direct_input)
    );
    task ticks(input integer n); repeat(n) begin @(posedge clk);#1;end endtask
    task start_read(input [15:0] a);
        @(negedge clk);read_address=a;read_start=1;
        @(negedge clk);read_start=0;
    endtask
    task reject_direct_burst(input[15:0]a,input[7:0]v,input flash_tag);
        integer n;
        begin
            @(negedge clk);direct_address=a;direct_value=v;direct_flash=flash_tag;direct_start=1;
            @(negedge clk);direct_start=0;
            n=0;
            while(!direct_done&&n<30)begin ticks(1);n=n+1;end
            if(!direct_done||!direct_failed||!direct_wr||!direct_input)
                $fatal(1,"Backend burst accepted a prohibited low/untagged address");
        end
    endtask
    task wait_done;
        integer n;
        begin n=0;while(!read_done&&n<30)begin ticks(1);n=n+1;end
            if(!read_done)$fatal(1,"Missing read completion");
        end
    endtask
    integer powered_ticks=0,rd_ticks=0;
    reg previous_rd=1,previous_control=0, abort_expected=0;
    always @(posedge clk) begin
        #1;
        if (!wr_n||!cs_n||!data_input) $fatal(1,"Read-only bus drove a write, RAM select or data output");
        if (disabled_power||disabled_control||disabled_ready||!disabled_wr||!disabled_input)
            $fatal(1,"Default-disabled cartridge became active during a valid mapper-write request");
        if(!direct_wr||!direct_input)$fatal(1,"Direct invalid burst drove WR or data");
        if(reset)begin powered_ticks=0;rd_ticks=0;previous_rd=1;previous_control=0;end
        else begin
            if(power_enable)powered_ticks=powered_ticks+1;else powered_ticks=0;
            if(control_enable&&!previous_control&&powered_ticks<4)$fatal(1,"Isolation enabled before power settled");
            if(!rd_n)begin rd_ticks=rd_ticks+1;if(!ready)$fatal(1,"Read strobe while not ready");end
            if(!previous_rd&&rd_n)begin
                if(!abort_expected&&rd_ticks!=5)$fatal(1,"Incorrect read pulse duration");
                rd_ticks=0;
            end
            previous_rd=rd_n;previous_control=control_enable;
        end
    end
    initial begin
        ticks(2);@(negedge clk);reset=0;
        start_read(16'h0134);wait_done();
        if(!read_failed||!rd_n)$fatal(1,"Unpowered read was accepted");
        @(negedge clk);switch_on=1;present=1;
        ticks(20);
        if(!ready||!power_enable||!control_enable||reset_low)$fatal(1,"Power/reset sequence did not reach ready");
        start_read(16'h0134);wait_done();
        if(read_failed||read_value!=8'h61)$fatal(1,"Header read data incorrect");
        @(negedge clk);read_address=16'h4055;write_value=8'h98;write_is_flash=1;write_start=1;
        @(negedge clk);write_start=0;wait_done();
        if(!read_failed||!wr_n||!data_input)$fatal(1,"Backend ROM gate permitted a command write");
        // Even an address/value otherwise valid as a mapper write must be
        // rejected when explicitly tagged as a disabled flash command.
        @(negedge clk);read_address=16'h4000;write_value=0;write_start=1;
        @(negedge clk);write_start=0;wait_done();
        if(!read_failed||!wr_n||!data_input)$fatal(1,"ROM gate fell through to the mapper whitelist");
        reject_direct_burst(16'h0000,8'hf0,1);
        reject_direct_burst(16'h00aa,8'h98,1);
        reject_direct_burst(16'h0555,8'h55,1);
        reject_direct_burst(16'h0aaa,8'haa,1);
        reject_direct_burst(16'h8000,8'hc3,1);
        reject_direct_burst(16'ha000,8'hc3,1);
        reject_direct_burst(16'h4000,8'h00,0);
        write_is_flash=0;
        start_read(16'h8000);wait_done();
        if(!read_failed||!rd_n)$fatal(1,"Non-ROM read was accepted");
        abort_expected=1;start_read(16'h0140);wait(!rd_n);
        @(negedge clk);present=0;
        wait_done();
        if(!read_failed||!rd_n||control_enable)$fatal(1,"Removal did not isolate/abort the bus");
        ticks(5);if(power_enable||ready)$fatal(1,"Power remained on after shutdown");
        @(negedge clk);present=1;legacy_strap=1;
        ticks(20);if(power_enable||control_enable||ready)$fatal(1,"Mismatched board revision was powered");
        @(negedge clk);legacy_strap=0;switch_on=0;
        ticks(5);if(power_enable||control_enable)$fatal(1,"Switch-off activated cartridge");
        $display("PASS: original read-only cartridge sequencing, disabled default, revision guard, reads and removal");
        $finish;
    end
    initial begin #20000;$fatal(1,"Timeout");end
endmodule
