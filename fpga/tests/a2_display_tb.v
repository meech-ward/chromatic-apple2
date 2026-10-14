// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_display_tb;
    reg clk=0;always #5 clk=~clk;
    reg reset=1,enabled=1;
    reg frame_ready=0;
    reg [3:0] pixel_data=0;
    wire [14:0] pixel_address;
    // Model the framebuffer's one-clock read latency with alternating pixels.
    always @(posedge clk) pixel_data<=pixel_address[0] ? 4'hf : 4'h0;
    wire dotclk,hsync,vsync,data_enable;
    wire [5:0]data;
    wire [9:0]scan_x;
    wire [8:0]scan_y;
    a2_display dut(.*);
    integer frame_count=0,pixels=0,lines=0,active_transfers=0;
    integer prev_x=-1,prev_y=-1;
    reg checking=0;
    time last_output_change=0;
    always @(data or hsync or vsync or data_enable or scan_x or scan_y)begin
        last_output_change=$time;
        #1;
        if(checking&&!reset&&dotclk!==1'b1)
            $fatal(1,"Panel signals changed outside rising DOTCLK launch phase");
    end
    always @(negedge dotclk) begin
        #1;
        if(!reset)begin
            if(checking&&$time-last_output_change<20)
                $fatal(1,"Panel data/sync lacked two core-clock periods of setup before capture");
            if(scan_x==0&&scan_y==0)begin
                if(checking)begin
                    if(pixels!=912*158||lines!=158||active_transfers!=720*144)
                        $fatal(1,"Bad frame geometry clocks=%0d lines=%0d transfers=%0d",pixels,lines,active_transfers);
                    frame_count=frame_count+1;
                    if(frame_count==2)frame_ready=1;
                    if(frame_count==3)begin
                        $display("PASS: falling-edge panel capture, LCD geometry, RGB phases and framebuffer alignment");$finish;
                    end
                end
                checking=1;pixels=0;lines=0;active_transfers=0;prev_x=-1;prev_y=-1;
            end
            if(checking)begin
                pixels=pixels+1;
                if(scan_x==0)lines=lines+1;
                if(hsync!==(scan_x>=30)||vsync!==(scan_y>=2))$fatal(1,"Bad sync polarity/window");
                if(data_enable!== (enabled&&scan_x>=65&&scan_x<785&&scan_y>=4&&scan_y<148))
                    $fatal(1,"Bad active-data window");
                if(prev_x>=0&&((prev_x==911&&scan_x!=0)||(prev_x!=911&&scan_x!=prev_x+1)))
                    $fatal(1,"Horizontal counter skipped");
                if(data_enable)begin
                    active_transfers=active_transfers+1;
                    if(frame_ready)begin
                        if(data!==((scan_x>=185&&scan_x<665&&((scan_x-185)/3)%2==1)?6'h3f:6'h00))
                            $fatal(1,"Framebuffer latency/address mismatch at %0d,%0d data=%h",scan_x,scan_y,data);
                    end else if(data!==(((scan_x<305&&(scan_x-65)%3==0)||(scan_x>=305&&scan_x<545&&(scan_x-65)%3==1)||(scan_x>=545&&(scan_x-65)%3==2))?6'h3f:6'h00))
                        $fatal(1,"RGB component phase/data incorrect at %0d",scan_x);
                end else if(data!=0)$fatal(1,"Nonzero blanking data");
                prev_x=scan_x;prev_y=scan_y;
            end
        end
    end
    initial begin repeat(3)@(posedge clk);@(negedge clk);reset=0;end
    initial begin #30000000;$fatal(1,"Timeout");end
endmodule
