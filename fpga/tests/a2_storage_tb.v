// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_storage_tb #(parameter FLASH_WRITES=0);
    reg clk=0;always #5 clk=~clk;
    reg reset=1,switch_on=1,present=1,legacy_strap=0;
    reg req_valid=0,response_ack=0;
    reg [7:0]req_length=5;
    reg [295:0]req_data=0;
    wire response_valid,cart_start,cart_write_start,cart_ready,cart_done,cart_failed;
    wire [287:0]response_data;
    wire [7:0]response_length,cart_write_value,cart_value;
    wire [15:0]cart_address,address;
    wire power_enable,control_enable,reset_low,cs_n,rd_n,wr_n,data_input;
    wire [7:0]data_out;
    wire cart_write_is_flash,cart_write_is_burst;
    reg [7:0]ram[0:32767];
    reg [1:0]ram_bank=0;
    reg [7:0]rom_bank=1;
    reg ram_enabled=0;
    wire [14:0]ram_offset={ram_bank,address[12:0]};
    wire [7:0]data_in=cs_n ? (address[7:0]^(address<16'h4000 ? 8'h5a : rom_bank)) :
                                      (ram_enabled ? ram[ram_offset] : 8'hff);
    a2_control #(.FLASH_WRITES(FLASH_WRITES)) control(.clk(clk),.reset(reset),.req_valid(req_valid),.req_length(req_length),.req_data(req_data),
        .response_valid(response_valid),.response_data(response_data),.response_length(response_length),.response_ack(response_ack),
        .buttons(9'd0),.menu_sequence(8'd0),.flags(8'd0),.straps(3'd0),.cart_ready(cart_ready),.cart_start(cart_start),
        .lcd_readback(48'd0),.lcd_readback_flags(2'd0),
        .lcd_te_level(1'b0),.lcd_te_edges(16'd0),
        .audio_ready(1'b0),.audio_flags(8'd0),.audio_error(8'd0),.audio_page(8'd0),
        .audio_register(8'd0),.audio_last_read(8'd0),.audio_readback(72'd0),
        .audio_din_ones(16'd0),.audio_frame_count(16'd0),
        .cart_write_start(cart_write_start),.cart_write_value(cart_write_value),.cart_address(cart_address),
        .cart_write_is_flash(cart_write_is_flash),.cart_write_is_burst(cart_write_is_burst),
        .cart_done(cart_done),.cart_failed(cart_failed),.cart_value(cart_value));
    a2_cartridge #(.CART_MODE(2),.FLASH_WRITES(FLASH_WRITES),.POWER_TICKS(4),.RESET_TICKS(2),.SETTLE_TICKS(4),.SHUTDOWN_TICKS(3),
                  .SETUP_TICKS(3),.READ_TICKS(5),.WRITE_TICKS(4),.HOLD_TICKS(2)) cartridge(
        .clk(clk),.reset(reset),.switch_on(switch_on),.present(present),.legacy_strap(legacy_strap),.ready(cart_ready),
        .read_start(cart_start),.read_address(cart_address),.write_start(cart_write_start),.write_value(cart_write_value),
        .write_is_flash(cart_write_is_flash),.write_is_burst(cart_write_is_burst),
        .read_done(cart_done),.read_failed(cart_failed),.read_value(cart_value),.data_in(data_in),.data_out(data_out),
        .address(address),.power_enable(power_enable),.control_enable(control_enable),.reset_low(reset_low),
        .cs_n(cs_n),.rd_n(rd_n),.wr_n(wr_n),.data_input(data_input));
    integer i,writes=0,write_ticks=0,hold_ticks=0;
    reg last_wr=1,last_input=1,aborting=0;
    reg[15:0]held_address=0;
    reg[7:0]held_value=0;
    reg[15:0]written_address[0:1023];
    reg[7:0]written_value[0:1023];
    always @(posedge clk)begin
        #1;
        if(!reset)begin
            if(!rd_n&&!wr_n)$fatal(1,"Read and write active together");
            if(!rd_n&&(!data_input||cs_n!==(address[15:13]!=3'b101)))$fatal(1,"Read direction/selection incorrect");
            if(!wr_n)begin
                if(data_input||!cart_ready)$fatal(1,"Write without output direction/readiness");
                if(write_ticks==0)begin held_address=address;held_value=data_out;end
                else if(address!=held_address||data_out!=held_value)$fatal(1,"Address/data changed inside write");
                write_ticks=write_ticks+1;
                if(address[15:13]==3'b101)begin if(cs_n)$fatal(1,"SRAM write not selected");end
                else if(!cs_n||!((FLASH_WRITES!=0&&((address>=16'h4000&&address<16'h8000)||
                    (address==0&&data_out==8'hf0)||(address==16'h00aa&&data_out==8'h98)||
                    (address==16'h0555&&data_out==8'h55)||(address==16'h0aaa&&
                     (data_out==8'haa||data_out==8'h90||data_out==8'ha0||data_out==8'h80))))||
                    (address==0&&(data_out==0||data_out==10))||address==16'h2000||
                    (address==16'h3000&&data_out==0)||(address==16'h4000&&data_out<=3)))
                    $fatal(1,"Write escaped mapper/SRAM whitelist");
            end
            if(!last_wr&&wr_n)begin
                if(!aborting&&write_ticks!=4)$fatal(1,"Wrong write pulse duration");
                written_address[writes]=address;written_value[writes]=data_out;
                writes=writes+1;write_ticks=0;hold_ticks=0;
                if(!aborting)begin
                    if(data_input)$fatal(1,"Data released at WR deassertion");
                    if(address[15:13]==3'b101)begin if(ram_enabled)ram[ram_offset]=data_out;end
                    else case(address)
                        0:ram_enabled=data_out==10;
                        16'h2000:rom_bank=data_out;
                        16'h4000:ram_bank=data_out[1:0];
                    endcase
                end
            end else if(wr_n&&!data_input)hold_ticks=hold_ticks+1;
            if(!last_input&&data_input&&!aborting&&hold_ticks<1)$fatal(1,"Write data hold interval missing");
            last_wr=wr_n;last_input=data_input;
        end
    end
    task ticks(input integer n);repeat(n)begin @(posedge clk);#2;end endtask
    task send(input[7:0]tag,op,input[15:0]addr,input[7:0]value);
        @(negedge clk);req_data={tag,op,addr,value,256'd0};req_length=5;req_valid=1;
        @(negedge clk);req_valid=0;
    endtask
    task send_burst(input[7:0]tag,input[15:0]addr,input[7:0]count,len);
        integer n;reg[295:0]payload;
        begin
            payload={tag,8'd7,addr,count,256'd0};
            for(n=0;n<32;n=n+1)payload[255-n*8 -:8]=8'ha5^n;
            @(negedge clk);req_data=payload;req_length=len;req_valid=1;
            @(negedge clk);req_valid=0;
        end
    endtask
    task burst_response(input[7:0]tag,input[15:0]addr,input[7:0]count,status);
        begin
            wait_response();
            if(response_length!=5||response_data[287:248]!=={tag,status,addr,count})
                $fatal(1,"Burst response mismatch got%h",response_data[287:248]);
        end
    endtask
    task wait_response;
        integer n;begin n=0;while(!response_valid&&n<1000)begin ticks(1);n=n+1;end
            if(!response_valid)$fatal(1,"Missing response");end
    endtask
    task ack;
        @(negedge clk);response_ack=1;@(negedge clk);response_ack=0;ticks(1);
    endtask
    task single(input[7:0]tag,op,input[15:0]addr,input[7:0]value,status,result);
        send(tag,op,addr,value);wait_response();
        if(response_length!=5||response_data[287:248]!=={tag,status,addr,result})
            $fatal(1,"Single response mismatch tag%0d got%h",tag,response_data[287:248]);
        ack();
    endtask
    integer previous_writes;
    reg[287:0]saved_response;
    initial begin
        for(i=0;i<32768;i=i+1)ram[i]=0;
        ticks(2);@(negedge clk);reset=0;ticks(25);
        if(!cart_ready)$fatal(1,"Cartridge never ready");
        single(1,0,16'h0134,0,0,8'h6e);
        single(2,0,16'ha000,0,0,8'hff);
        previous_writes=writes;
        single(3,1,16'h5555,8'haa,2,8'haa);
        single(4,1,16'h0000,1,2,1);
        single(5,1,16'h3000,1,2,1);
        single(6,1,16'h4000,4,2,4);
        single(7,4,16'h2000,8'h99,2,8'h99);
        if(writes!=previous_writes)$fatal(1,"Rejected command asserted WR");
        if(FLASH_WRITES==0)begin
            single(70,6,16'h4055,8'h98,1,8'h98);
            if(writes!=previous_writes)$fatal(1,"Default-disabled ROM command asserted WR");
            send_burst(92,16'h4000,32,37);burst_response(92,16'h4000,32,1);ack();
            if(writes!=previous_writes)$fatal(1,"Default-disabled ROM burst asserted WR");
        end else begin
            single(70,6,16'h3fff,8'h98,2,8'h98);
            single(71,6,16'h8000,8'h98,2,8'h98);
            single(72,6,16'ha000,8'h98,2,8'h98);
            if(writes!=previous_writes)$fatal(1,"Out-of-window ROM command asserted WR");
            single(73,6,16'h4055,8'h98,0,8'h98);
            single(74,6,16'h4000,8'hf0,0,8'hf0);
            single(75,6,16'h7fff,8'hf0,0,8'hf0);
            single(80,6,16'h0000,8'hf0,0,8'hf0);
            single(81,6,16'h00aa,8'h98,0,8'h98);
            single(82,6,16'h0555,8'h55,0,8'h55);
            single(83,6,16'h0aaa,8'haa,0,8'haa);
            single(84,6,16'h0aaa,8'h90,0,8'h90);
            single(85,6,16'h0aaa,8'ha0,0,8'ha0);
            single(86,6,16'h0aaa,8'h80,0,8'h80);
            previous_writes=writes;
            single(87,6,16'h0000,0,2,0);
            single(88,6,16'h00aa,8'h99,2,8'h99);
            single(89,6,16'h0555,8'h54,2,8'h54);
            single(90,6,16'h0aaa,8'hf0,2,8'hf0);
            single(91,6,16'h0aab,8'haa,2,8'haa);
            if(writes!=previous_writes)$fatal(1,"Unlisted low command pair asserted WR");
            send_burst(92,16'h4100,32,37);
            // A following request must not alter latched burst data or address.
            wait(writes==previous_writes+1);
            send(93,4,16'ha000,8'hff);
            burst_response(92,16'h4100,32,0);
            if(writes!=previous_writes+32)$fatal(1,"ROM burst issued wrong write count");
            for(i=0;i<32;i=i+1)begin
                if(written_address[previous_writes+i]!==16'h4100+i||
                   written_value[previous_writes+i]!==(8'ha5^i[7:0]))
                    $fatal(1,"ROM burst byte/address mismatch at %0d",i);
            end
            saved_response=response_data;previous_writes=writes;
            send(94,4,16'ha000,8'hff);ticks(30);
            if(response_data!==saved_response||writes!=previous_writes)
                $fatal(1,"Burst response backpressure allowed overwrite or another write");
            ack();
            send_burst(95,16'h7fff,1,6);burst_response(95,16'h7fff,1,0);ack();
            if(writes!=previous_writes+1||written_address[previous_writes]!==16'h7fff||
               written_value[previous_writes]!==8'ha5)$fatal(1,"Single-byte burst incorrect");
            previous_writes=writes;
            send_burst(96,16'h7fff,2,7);burst_response(96,16'h7fff,2,2);ack();
            send_burst(97,16'h3fff,1,6);burst_response(97,16'h3fff,1,2);ack();
            send_burst(98,16'h8000,1,6);burst_response(98,16'h8000,1,2);ack();
            send_burst(99,16'ha000,1,6);burst_response(99,16'ha000,1,2);ack();
            send_burst(100,16'h0000,1,6);burst_response(100,16'h0000,1,2);ack();
            if(writes!=previous_writes)$fatal(1,"Invalid burst range drove WR");
        end
        previous_writes=writes;
        send_burst(101,16'h4000,0,5);burst_response(101,16'h4000,0,4);ack();
        send_burst(102,16'h4000,33,38);burst_response(102,16'h4000,33,4);ack();
        send_burst(103,16'h4000,32,36);burst_response(103,16'h4000,32,4);ack();
        if(writes!=previous_writes)$fatal(1,"Malformed burst drove WR");
        single(8,1,16'h0000,10,0,10);
        single(9,1,16'h2000,7,0,7);
        single(10,1,16'h3000,0,0,0);
        for(i=0;i<4;i=i+1)begin
            single(20+i*3,1,16'h4000,i,0,i);
            single(21+i*3,4,16'ha000,8'hd0+i,0,8'hd0+i);
            single(22+i*3,0,16'ha000,0,0,8'hd0+i);
        end
        single(40,4,16'hbfff,8'hc3,0,8'hc3);
        single(41,0,16'hbfff,0,0,8'hc3);
        send(42,5,16'h4110,32);wait_response();
        if(response_length!=36||response_data[287:256]!=={8'd42,8'd0,16'h4110})$fatal(1,"Bulk header mismatch");
        for(i=0;i<32;i=i+1)if(response_data[255-i*8 -:8]!==((8'h10+i)^8'd7))$fatal(1,"Bulk byte%0d mismatch",i);
        saved_response=response_data;previous_writes=writes;
        send(43,4,16'ha000,8'hff);ticks(30);
        if(response_data!==saved_response||writes!=previous_writes)$fatal(1,"Outstanding bulk response overwritten");
        ack();
        single(44,5,16'h7fff,2,2,2);
        single(45,5,16'hbfff,2,2,2);
        single(46,5,16'ha000,0,4,0);
        single(47,5,16'ha000,33,4,33);
        for(i=0;i<4;i=i+1)begin
            single(50+i*2,1,16'h4000,i,0,i);
            single(51+i*2,0,16'ha000,0,0,8'hd0+i);
        end
        aborting=1;send(60,5,16'h4200,32);wait(!rd_n);
        @(negedge clk);present=0;wait_response();
        if(response_length!=5||response_data[287:248]!=={8'd60,8'd3,16'h4200,8'd0})$fatal(1,"Removal bulk error invalid");
        ack();ticks(6);if(power_enable||control_enable||!wr_n||!rd_n||!data_input)$fatal(1,"Removal left bus active");
        @(negedge clk);present=1;ticks(25);aborting=0;
        if(FLASH_WRITES!=0)begin
            previous_writes=writes;
            send_burst(104,16'h4100,32,37);wait(writes==previous_writes+3);aborting=1;
            @(negedge clk);present=0;
            burst_response(104,16'h4100,32,3);ack();ticks(6);
            if(power_enable||control_enable||!wr_n||!data_input)$fatal(1,"Burst removal left bus driven");
            previous_writes=writes;ticks(30);
            if(writes!=previous_writes)$fatal(1,"Burst continued after removal");
            @(negedge clk);present=1;ticks(25);aborting=0;
            send(76,6,16'h4055,8'h98);wait(!wr_n);aborting=1;
            @(negedge clk);present=0;wait_response();
            if(response_data[279:272]!=3)$fatal(1,"ROM command removal not reported");
            ack();ticks(6);
            if(power_enable||control_enable||!wr_n||!data_input)$fatal(1,"ROM command removal left bus driven");
            @(negedge clk);present=1;ticks(25);aborting=0;
        end
        send(61,4,16'ha002,8'hfe);wait(!wr_n);aborting=1;
        @(negedge clk);switch_on=0;wait_response();
        if(response_data[279:272]!=3)$fatal(1,"Write power abort not reported");
        ack();ticks(6);if(power_enable||control_enable||!wr_n||!data_input)$fatal(1,"Power abort left bus active");
        $display("PASS: storage, bounded bulk, timing, ROM command gate=%0d, bounds and aborts",FLASH_WRITES);$finish;
    end
    initial begin #200000;$fatal(1,"Timeout");end
endmodule
