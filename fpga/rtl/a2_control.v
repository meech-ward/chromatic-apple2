// SPDX-License-Identifier: MIT
// Five-byte request headers; operation7 appends1..32 sequential write bytes.
module a2_control #(parameter FLASH_WRITES=0) (
    input wire clk, input wire reset,
    input wire req_valid, input wire [7:0] req_length, input wire [295:0] req_data,
    output reg response_valid, output reg [287:0] response_data,
    output reg [7:0] response_length, input wire response_ack,
    input wire [8:0] buttons, input wire [7:0] menu_sequence,
    input wire [7:0] flags, input wire [2:0] straps,
    input wire [47:0] lcd_readback, input wire [1:0] lcd_readback_flags,
    input wire lcd_te_level, input wire [15:0] lcd_te_edges,
    input wire audio_ready, output reg audio_arm,
    input wire [7:0] audio_flags,audio_error,audio_page,audio_register,audio_last_read,
    input wire [71:0] audio_readback,
    input wire [15:0] audio_din_ones,audio_frame_count,
    input wire cart_ready,
    output reg cart_start, output reg cart_write_start,
    output reg cart_write_is_flash, output reg cart_write_is_burst,
    output reg [7:0] cart_write_value, output reg [15:0] cart_address,
    input wire cart_done, input wire cart_failed, input wire [7:0] cart_value
);
    reg waiting=0, bulk=0, burst_write=0;
    reg [255:0] write_payload=0;
    reg [7:0] token=0, bulk_count=0, bulk_index=0;
    reg [15:0] original_address=0;
    wire [7:0] op=req_data[287:280];
    wire [15:0] addr=req_data[279:264];
    wire [7:0] value=req_data[263:256];
    wire ram_address=addr[15:13]==3'b101;
    wire read_address=addr<16'h8000 || ram_address;
    wire mapper_write=(addr==16'h0000 && (value==0 || value==8'h0a)) || addr==16'h2000 ||
                      (addr==16'h3000 && value==0) || (addr==16'h4000 && value<=3);
    wire flash_address=(addr>=16'h4000 && addr<16'h8000) ||
                       (addr==0 && value==8'hf0) || (addr==16'h00aa && value==8'h98) ||
                       (addr==16'h0555 && value==8'h55) || (addr==16'h0aaa &&
                         (value==8'haa || value==8'h90 || value==8'ha0 || value==8'h80));
    wire [16:0] bulk_end={1'b0,addr}+{9'd0,value}-1'b1;
    wire bulk_range=(addr<16'h8000 && bulk_end<17'h08000) ||
                    (ram_address && bulk_end<=17'h0bfff);
    wire burst_range=addr>=16'h4000 && bulk_end<17'h08000;
    function [287:0] single_response;
        input [7:0] tag,status;
        input [15:0] address;
        input [7:0] result;
        begin single_response={tag,status,address,result,248'd0};end
    endfunction
    reg [7:0] diagnostic;
    always @* begin
        case(addr)
            0: diagnostic=8'ha2;
            1: diagnostic=buttons[7:0];
            2: diagnostic={7'd0,buttons[8]};
            3: diagnostic=flags;
            4: diagnostic={5'd0,straps};
            5: diagnostic=lcd_readback[47:40];
            6: diagnostic=lcd_readback[39:32];
            7: diagnostic=lcd_readback[31:24];
            8: diagnostic=lcd_readback[23:16];
            9: diagnostic=lcd_readback[15:8];
            10: diagnostic=lcd_readback[7:0];
            11: diagnostic={6'd0,lcd_readback_flags};
            12: diagnostic={7'd0,lcd_te_level};
            13: diagnostic=lcd_te_edges[7:0];
            14: diagnostic=lcd_te_edges[15:8];
            15: diagnostic=audio_flags;
            16: diagnostic=audio_error;
            17: diagnostic=audio_page;
            18: diagnostic=audio_register;
            19: diagnostic=audio_last_read;
            20: diagnostic=audio_readback[71:64];
            21: diagnostic=audio_readback[63:56];
            22: diagnostic=audio_readback[55:48];
            23: diagnostic=audio_readback[47:40];
            24: diagnostic=audio_readback[39:32];
            25: diagnostic=audio_readback[31:24];
            26: diagnostic=audio_readback[23:16];
            27: diagnostic=audio_readback[15:8];
            28: diagnostic=audio_readback[7:0];
            29: diagnostic=audio_din_ones[7:0];
            30: diagnostic=audio_din_ones[15:8];
            31: diagnostic=audio_frame_count[7:0];
            32: diagnostic=audio_frame_count[15:8];
            33: diagnostic=menu_sequence;
            default: diagnostic=0;
        endcase
    end
    always @(posedge clk) begin
        if (reset) begin
            waiting<=0; bulk<=0; burst_write<=0; write_payload<=0;
            token<=0; bulk_count<=0; bulk_index<=0; original_address<=0;
            response_valid<=0; response_data<=0; response_length<=5;
            cart_start<=0; cart_write_start<=0; cart_write_is_flash<=0; cart_write_is_burst<=0;
            cart_write_value<=0; cart_address<=0;
            audio_arm<=0;
        end else begin
            cart_start<=0; cart_write_start<=0;
            if (!audio_ready) audio_arm<=0;
            if (response_valid) begin
                if (response_ack) response_valid<=0;
            end else if (waiting) begin
                if (cart_done) begin
                    if (cart_failed) begin
                        waiting<=0; response_valid<=1; response_length<=5;
                        response_data<=single_response(token,3,original_address,burst_write ? bulk_count : 8'd0);
                    end else if (burst_write) begin
                        if (bulk_index+1==bulk_count) begin waiting<=0; response_valid<=1; end
                        else begin
                            bulk_index<=bulk_index+1'b1; cart_address<=cart_address+1'b1;
                            cart_write_value<=write_payload[255:248];
                            write_payload<={write_payload[247:0],8'd0}; cart_write_start<=1;
                        end
                    end else if (!bulk) begin
                        waiting<=0; response_valid<=1; response_length<=5;
                        response_data<=single_response(token,0,original_address,cart_value);
                    end else begin
                        response_data[255-bulk_index*8 -: 8]<=cart_value;
                        if (bulk_index+1==bulk_count) begin waiting<=0; response_valid<=1; end
                        else begin bulk_index<=bulk_index+1'b1; cart_address<=cart_address+1'b1; cart_start<=1; end
                    end
                end
            end else if (req_valid) begin
                response_data<=single_response(req_data[295:288],0,addr,value);
                response_length<=5; response_valid<=1;
                if ((op==7 && (req_length<6 || req_length>37 || req_length!={1'b0,value}+9'd5)) ||
                    (op!=7 && req_length!=5)) response_data[279:272]<=4;
                else if (op==2) ;
                else if (op==3) begin
                    response_data[255:248]<=diagnostic;
                    if (addr>33) response_data[279:272]<=2;
                end else if (op==8) begin
                    if (addr!=0 || value>1) response_data[279:272]<=2;
                    else if (value==1 && !audio_ready) response_data[279:272]<=3;
                    else audio_arm<=value[0];
                end else if ((op!=0 && op!=1 && op!=4 && op!=5 && op!=6 && op!=7) ||
                             ((op==6 || op==7) && FLASH_WRITES==0)) response_data[279:272]<=1;
                else if ((op==5 || op==7) && (value==0 || value>32)) response_data[279:272]<=4;
                else if ((op==0 && !read_address) || (op==1 && !mapper_write) ||
                         (op==4 && !ram_address) || (op==5 && !bulk_range) ||
                         (op==6 && !flash_address) || (op==7 && !burst_range)) response_data[279:272]<=2;
                else if (!cart_ready) response_data[279:272]<=3;
                else begin
                    token<=req_data[295:288]; original_address<=addr;
                    cart_address<=addr; cart_write_value<=op==7 ? req_data[255:248] : value;
                    cart_start<=op==0 || op==5; cart_write_start<=op==1 || op==4 || op==6 || op==7;
                    cart_write_is_flash<=op==6 || op==7; cart_write_is_burst<=op==7;
                    bulk<=op==5; burst_write<=op==7; waiting<=1; response_valid<=0;
                    if (op==7) begin
                        bulk_count<=value; bulk_index<=0; write_payload<={req_data[247:0],8'd0};
                    end
                    if (op==5) begin
                        bulk_count<=value; bulk_index<=0; response_length<=4+value;
                        response_data<={req_data[295:288],8'd0,addr,256'd0};
                    end
                end
            end
        end
    end
endmodule
