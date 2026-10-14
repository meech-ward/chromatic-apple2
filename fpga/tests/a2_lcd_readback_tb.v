// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_lcd_readback_tb;
    reg clk=0;always #5 clk=~clk;
    reg reset=1;
    wire init_cs,init_sck,init_sda,lcd_reset,init_done;
    a2_lcd_init #(.CLOCK_HZ(1000),.SPI_HALF(4)) init_dut(
        .clk(clk),.reset(reset),.lcd_reset(lcd_reset),.cs_n(init_cs),.sck(init_sck),.sda(init_sda),.done(init_done));
    wire cs_n,sck,sda_out,sda_oe,done,plausible;
    wire[7:0]power_mode,orientation,pixel_format,id1,id2,id3;
    tri1 sda;
    wire bus_cs=init_done?cs_n:init_cs;
    wire bus_sck=init_done?sck:init_sck;
    assign sda=!init_done?init_sda:sda_oe?sda_out:1'bz;
    reg panel_oe=0,panel_bit=1;
    assign sda=panel_oe?panel_bit:1'bz;
    a2_lcd_readback #(.SPI_HALF(4)) dut(.clk(clk),.reset(reset),.start(init_done),.sda_in(sda),
        .cs_n(cs_n),.sck(sck),.sda_out(sda_out),.sda_oe(sda_oe),.done(done),.plausible(plausible),
        .power_mode(power_mode),.orientation(orientation),.pixel_format(pixel_format),.id1(id1),.id2(id2),.id3(id3));
    integer command_bits=0,read_samples=0,transactions=0,init_edges=0,model_mode=0;
    reg[8:0]command=0;
    reg[7:0]reply=0;
    reg read_session=0;
    time released_at=0;
    function[7:0]register_value;
        input[8:0]c;
        begin case(c)
            9'h00a:register_value=8'h9c;
            9'h00b:register_value=0;
            9'h00c:register_value=8'h66;
            9'h0da:register_value=8'h85;
            9'h0db:register_value=8'h85;
            9'h0dc:register_value=8'h52;
            default:begin $fatal(1,"Unexpected/mutating LCD command%h",c);register_value=0;end
        endcase end
    endfunction
    always @(posedge clk)if(reset)begin
        command_bits=0;read_samples=0;transactions=0;init_edges=0;panel_oe=0;read_session=0;
    end
    always @(negedge bus_cs)if(!reset&&init_done)begin
        read_session=1;command_bits=0;read_samples=0;command=0;panel_oe=0;
    end
    always @(posedge bus_sck)if(!reset)begin
        if(!init_done)begin
            init_edges=init_edges+1;
            if(sda_oe||!cs_n||sck)$fatal(1,"Readback interfered with initialization");
        end else begin
            if(bus_cs)$fatal(1,"Probe clock while CS inactive");
            if(command_bits<9)begin
                if(!sda_oe||panel_oe)$fatal(1,"Command data direction invalid");
                command={command[7:0],sda};command_bits=command_bits+1;
                if(command_bits==9)reply=register_value(command);
            end else begin
                if(sda_oe)$fatal(1,"Host drove SDA during LCD reply");
                read_samples=read_samples+1;
                if(read_samples>8)$fatal(1,"Unbounded read clocks");
            end
        end
    end
    always @(negedge sda_oe)if(read_session&&command_bits==9)released_at=$time;
    always @(negedge bus_sck)if(!reset&&read_session&&!bus_cs&&command_bits==9)begin
        if(sda_oe||released_at>=$time)$fatal(1,"SDA was not released before the last command falling edge");
        if(read_samples<8)begin
            panel_oe=model_mode!=1;
            panel_bit=model_mode==2?0:reply[7-read_samples];
        end
    end
    always @(posedge bus_cs)begin
        if(!reset&&read_session)begin
            if(command_bits!=9||read_samples!=8)$fatal(1,"Incorrect bounded LCD transaction length");
            transactions=transactions+1;read_session=0;
        end
        panel_oe=0;
    end
    task reset_probe(input integer mode);
        begin
            @(negedge clk);reset=1;model_mode=mode;
            repeat(3)@(negedge clk);reset=0;
            wait(done);#1;
            if(transactions!=6||init_edges!=21*9||!cs_n||sck||sda_oe)$fatal(1,"Incomplete or nonquiet probe");
        end
    endtask
    initial begin
        reset_probe(0);
        if({power_mode,orientation,pixel_format,id1,id2,id3}!==48'h9c0066858552||!plausible)
            $fatal(1,"Readback bytes or consistency flag incorrect");
        repeat(100)@(posedge clk);
        if(transactions!=6)$fatal(1,"Readback repeated without reset");
        reset_probe(1);
        if({power_mode,orientation,pixel_format,id1,id2,id3}!==48'hffffffffffff||plausible)
            $fatal(1,"Floating-high line was not marked implausible");
        reset_probe(2);
        if({power_mode,orientation,pixel_format,id1,id2,id3}!==48'd0||plausible)
            $fatal(1,"Stuck-low line was not marked implausible");
        $display("PASS: init isolation,9-bit command handoff,8-bit replies, bounded reads and invalid-line flags");$finish;
    end
    initial begin #100000;$fatal(1,"Timeout");end
endmodule
