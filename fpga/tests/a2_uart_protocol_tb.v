// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_uart_protocol_tb #(parameter CLOCK_HZ=1843200, parameter BAUD=115200, parameter FLASH_WRITES=0);
    reg clk=0;always #5 clk=~clk;
    reg reset=1,rx=1;
    wire tx,rx_valid,tx_start,tx_busy,req_valid,response_valid,response_ack;
    wire [7:0]rx_data,tx_data,req_length;
    wire [295:0]req_data;
    wire [287:0]response_data;
    wire [7:0]response_length;
    a2_uart #(.CLOCK_HZ(CLOCK_HZ),.BAUD(BAUD)) uart(.*);
    // Host uses the requested baud, not the FPGA's rounded integer divider.
    localparam real BIT_TIME=10.0*CLOCK_HZ/BAUD;
    a2_packet packet(.*);
    wire cart_start;
    wire [15:0]cart_address;
    wire cart_write_start;
    wire cart_write_is_flash,cart_write_is_burst;
    wire [7:0]cart_write_value;
    reg cart_ready=0,cart_done=0,cart_failed=0;
    reg [7:0]cart_value=0,model_result=0;
    reg [3:0]model_delay=0;
    reg watching_burst=0;
    reg audio_ready=0;
    wire audio_arm;
    reg [15:0]burst_base=0;
    integer burst_writes=0;
    always @(posedge clk)begin
        cart_done<=0;
        if(cart_start||cart_write_start)begin
            if(watching_burst&&cart_write_start)begin
                if(!cart_write_is_flash||!cart_write_is_burst||cart_address!==burst_base+burst_writes||
                   cart_write_value!==(8'ha5^burst_writes[7:0]))
                    $fatal(1,"UART burst write address, value or flags incorrect");
                burst_writes=burst_writes+1;
            end
            model_delay<=4;model_result<=cart_write_start?cart_write_value:(cart_address[7:0]^8'h55);
        end else if(model_delay!=0)begin
            model_delay<=model_delay-1'b1;
            if(model_delay==1)begin cart_done<=1;cart_value<=model_result;end
        end
    end
    a2_control #(.FLASH_WRITES(FLASH_WRITES)) control(.clk(clk),.reset(reset),.req_valid(req_valid),.req_length(req_length),.req_data(req_data),
        .response_valid(response_valid),.response_data(response_data),.response_ack(response_ack),
        .response_length(response_length),.cart_write_start(cart_write_start),.cart_write_value(cart_write_value),
        .cart_write_is_flash(cart_write_is_flash),.cart_write_is_burst(cart_write_is_burst),
        .buttons(9'h12a),.menu_sequence(8'hfe),.flags(8'h1c),.straps(3'b101),.cart_ready(cart_ready),
        .lcd_readback(48'h9c0066858552),.lcd_readback_flags(2'b11),
        .lcd_te_level(1'b1),.lcd_te_edges(16'h1234),
        .audio_ready(audio_ready),.audio_arm(audio_arm),.audio_flags(8'h05),.audio_error(8'd0),
        .audio_page(8'd1),.audio_register(8'h40),.audio_last_read(8'h04),
        .audio_readback(72'h008182800094058690),.audio_din_ones(16'h1234),.audio_frame_count(16'h5678),
        .cart_start(cart_start),.cart_address(cart_address),.cart_done(cart_done),.cart_failed(cart_failed),.cart_value(cart_value));
    function [7:0] crc_byte;
        input [7:0]previous,value;integer i;reg[7:0]c;
        begin c=previous^value;for(i=0;i<8;i=i+1)c=c[7]?(c<<1)^8'h1d:c<<1;crc_byte=c;end
    endfunction
    task send_byte(input [7:0]v);
        integer i;
        begin
            @(negedge clk);rx=0;#(BIT_TIME);
            for(i=0;i<8;i=i+1)begin rx=v[i];#(BIT_TIME);end
            rx=1;#(BIT_TIME);
        end
    endtask
    task receive_byte(output [7:0]v);
        integer i;
        begin
            @(negedge tx);#(1.5*BIT_TIME);
            for(i=0;i<8;i=i+1)begin
                v[i]=tx;
                #(BIT_TIME);
            end
            if(!tx)$fatal(1,"Bad response stop bit");
        end
    endtask
    task send_packet(input [7:0]tag,op,input[15:0]addr,input[7:0]value,len,input bad_crc);
        reg[295:0]payload;
        begin
            payload={tag,op,addr,value,256'd0};
            send_payload(payload,len,bad_crc);
        end
    endtask
    task send_payload(input[295:0]payload,input[7:0]len,input bad_crc);
        integer i;reg[7:0]c,v;
        begin
            c=8'hff;
            send_byte(8'h8f);c=crc_byte(c,8'h8f);
            send_byte(8'h20);c=crc_byte(c,8'h20);
            send_byte(len);c=crc_byte(c,len);
            for(i=0;i<len;i=i+1)begin
                v=i<37 ? payload[295-i*8 -:8] : 8'hee;
                send_byte(v);c=crc_byte(c,v);
            end
            send_byte(bad_crc?(c^8'h01):c);
        end
    endtask
    task send_burst(input[7:0]tag,input[15:0]addr,input[7:0]count,len,input bad_crc);
        integer i;reg[295:0]payload;
        begin
            payload={tag,8'd7,addr,count,256'd0};
            for(i=0;i<32;i=i+1)payload[255-i*8 -:8]=8'ha5^i;
            send_payload(payload,len,bad_crc);
        end
    endtask
    task response(input[39:0]expected);
        integer i;reg[7:0]v,c;reg[39:0]payload;
        begin
            c=8'hff;payload=0;
            for(i=0;i<9;i=i+1)begin
                receive_byte(v);
                if(i==0&&v!=8'h8f)$fatal(1,"Missing response marker");
                if(i==1&&v!=8'h0a)$fatal(1,"Wrong response address");
                if(i==2&&v!=5)$fatal(1,"Wrong response length");
                if(i>=3&&i<=7)payload={payload[31:0],v};
                if(i<8)c=crc_byte(c,v);else if(v!=c)$fatal(1,"Bad response CRC");
            end
            if(payload!==expected)$fatal(1,"Response mismatch got=%h expected=%h",payload,expected);
        end
    endtask
    task transact(input[7:0]tag,op,input[15:0]addr,input[7:0]value,status,result);
        fork
            send_packet(tag,op,addr,value,5,0);
            response({tag,status,addr,result});
        join
        repeat(100)@(posedge clk);
        if(response_valid||cart_start)$fatal(1,"Response was not acknowledged exactly once");
    endtask
    task transact_burst(input[7:0]tag,input[15:0]addr,input[7:0]count,len,status);
        begin
            watching_burst=1;burst_base=addr;burst_writes=0;
            fork send_burst(tag,addr,count,len,0);response({tag,status,addr,count});join
            repeat(100)@(posedge clk);
            if(burst_writes!=(status==0 ? count : 0)||response_valid)
                $fatal(1,"UART burst count/ack mismatch");
            watching_burst=0;
        end
    endtask
    task bulk_response(input[7:0]tag,input[15:0]addr,input integer count);
        integer i;reg[7:0]v,c,expected;
        begin
            c=8'hff;
            for(i=0;i<count+8;i=i+1)begin
                receive_byte(v);
                case(i)
                    0:expected=8'h8f;1:expected=8'h0a;2:expected=4+count;
                    3:expected=tag;4:expected=0;5:expected=addr[15:8];6:expected=addr[7:0];
                    default:expected=i==count+7?c:((addr+i-7)^8'h55);
                endcase
                if(v!==expected)$fatal(1,"Bulk UART byte%0d expected%h got%h",i,expected,v);
                if(i<count+7)c=crc_byte(c,v);
            end
        end
    endtask
    reg forbid_tx=0;
    always @(negedge tx)if(forbid_tx)$fatal(1,"Unexpected/duplicate response");
    initial begin
        repeat(4)@(posedge clk);@(negedge clk);reset=0;
        transact(1,3,0,0,0,8'ha2);
        transact(2,3,1,0,0,8'h2a);
        transact(3,3,2,0,0,1);
        transact(4,3,3,0,0,8'h1c);
        transact(5,3,4,0,0,5);
        transact(6,3,34,0,2,0);
        transact(54,3,33,0,0,8'hfe);
        transact(55,3,33,0,0,8'hfe); // Reading does not consume a retained press.
        transact(23,3,5,0,0,8'h9c);
        transact(24,3,7,0,0,8'h66);
        transact(25,3,10,0,0,8'h52);
        transact(26,3,11,0,0,3);
        transact(27,3,12,0,0,1);
        transact(28,3,13,0,0,8'h34);
        transact(29,3,14,0,0,8'h12);
        transact(41,3,15,0,0,8'h05);
        transact(42,3,21,0,0,8'h81);
        transact(43,3,28,0,0,8'h90);
        transact(44,3,29,0,0,8'h34);
        transact(45,3,30,0,0,8'h12);
        transact(46,3,31,0,0,8'h78);
        transact(47,3,32,0,0,8'h56);
        transact(48,8,0,1,3,1);
        if(audio_arm)$fatal(1,"Audio armed before codec verification");
        audio_ready=1;transact(49,8,0,1,0,1);
        if(!audio_arm)$fatal(1,"Audio arm not accepted when ready");
        transact(50,8,1,1,2,1);
        transact(51,8,0,2,2,2);
        transact(52,8,0,0,0,0);
        if(audio_arm)$fatal(1,"Audio disarm ignored");
        transact(53,8,0,1,0,1);audio_ready=0;repeat(2)@(posedge clk);#1;
        if(audio_arm)$fatal(1,"Audio remained armed after codec readiness lost");
        transact(7,0,16'h0134,0,3,0);
        transact(8,1,16'h2000,8'h55,3,8'h55);
        transact(13,1,16'h5555,8'haa,2,8'haa);
        transact(9,2,0,0,0,0);
        fork send_packet(10,3,0,0,0,0);response({8'd0,8'd4,16'd0,8'd0});join
        repeat(100)@(posedge clk);
        forbid_tx=1;send_packet(11,3,0,0,5,1);repeat(2000)@(posedge clk);
        forbid_tx=0;
        transact(12,3,0,0,0,8'ha2);
        cart_ready=1;
        transact(14,1,16'h2000,8'h55,0,8'h55);
        transact(15,4,16'ha000,8'hc3,0,8'hc3);
        transact(22,6,16'h4055,8'h98,FLASH_WRITES==0 ? 1 : 0,8'h98);
        transact(16,5,16'h1000,0,4,0);
        transact(17,5,16'h1000,33,4,33);
        transact(18,5,16'h7ff0,32,2,32);
        transact(19,5,16'hbff0,32,2,32);
        fork send_packet(20,5,16'h1200,32,5,0);bulk_response(20,16'h1200,32);join
        repeat(100)@(posedge clk);
        fork send_packet(21,5,16'hbffc,4,5,0);bulk_response(21,16'hbffc,4);join
        repeat(100)@(posedge clk);
        transact_burst(30,16'h4000,1,6,FLASH_WRITES==0 ? 1 : 0);
        transact_burst(31,16'h7fe0,32,37,FLASH_WRITES==0 ? 1 : 0);
        transact_burst(32,16'h7fff,2,7,FLASH_WRITES==0 ? 1 : 2);
        transact_burst(33,16'h0000,1,6,FLASH_WRITES==0 ? 1 : 2);
        transact_burst(34,16'h4000,32,36,4);
        transact_burst(35,16'h4000,33,38,4);
        transact_burst(36,16'h4000,0,5,4);
        transact_burst(37,16'h4000,32,255,4);
        transact_burst(38,16'h4000,255,6,4);
        forbid_tx=1;send_burst(39,16'h4000,32,37,1);repeat(2000)@(posedge clk);
        forbid_tx=0;transact(40,3,0,0,0,8'ha2);
        forbid_tx=1;repeat(2000)@(posedge clk);
        $display("PASS: physical UART, CRC/length, diagnostics, bounded reads/writes, overflow rejection and no duplicates");$finish;
    end
    initial begin #20000000;$fatal(1,"Timeout");end
endmodule
