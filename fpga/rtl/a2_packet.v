// SPDX-License-Identifier: MIT
// Wire format: 8f, address, length, payload, CRC8(poly1d, initff, no xor).
// Requests use address20 and up to37 payload bytes; responses use address0a.
// Both payload buses are left-aligned; overlong requests retain only their header.
module a2_packet (
    input wire clk, input wire reset,
    input wire [7:0] rx_data, input wire rx_valid,
    output reg req_valid, output reg [7:0] req_length,
    output reg [295:0] req_data,
    input wire response_valid, input wire [287:0] response_data,
    input wire [7:0] response_length,
    output reg response_ack,
    output reg [7:0] tx_data, output reg tx_start, input wire tx_busy
);
    function [7:0] crc_byte;
        input [7:0] previous, value;
        integer b;
        reg [7:0] c;
        begin
            c=previous^value;
            for (b=0;b<8;b=b+1) c=c[7] ? (c<<1)^8'h1d : c<<1;
            crc_byte=c;
        end
    endfunction
    reg [2:0] receive_state=0;
    reg [7:0] receive_address=0, receive_left=0, receive_crc=8'hff;
    reg [7:0] receive_index=0;
    reg [2:0] transmit_state=0;
    reg [7:0] transmit_index=0, transmit_length=0;
    reg [287:0] transmit_payload=0;
    reg [7:0] transmit_crc=8'hff;
    reg [7:0] next_tx;
    always @* begin
        case (transmit_index)
            0: next_tx=8'h8f;
            1: next_tx=8'h0a;
            2: next_tx=transmit_length;
            default: next_tx=transmit_index==transmit_length+3 ? transmit_crc : transmit_payload[287:280];
        endcase
    end
    always @(posedge clk) begin
        if (reset) begin
            receive_state<=0; req_valid<=0; req_length<=0; req_data<=0;
            receive_address<=0; receive_left<=0; receive_crc<=8'hff;
            receive_index<=0;
            transmit_state<=0; transmit_index<=0; transmit_payload<=0;
            transmit_length<=0;
            transmit_crc<=8'hff; response_ack<=0; tx_start<=0; tx_data<=8'hff;
        end else begin
            req_valid<=0; response_ack<=0; tx_start<=0;
            if (rx_valid) begin
                case (receive_state)
                    0: if (rx_data==8'h8f) begin
                           receive_crc<=crc_byte(8'hff,rx_data); receive_state<=1;
                       end
                    1: begin
                           receive_address<=rx_data; receive_crc<=crc_byte(receive_crc,rx_data);
                           receive_state<=2;
                       end
                    2: begin
                           req_length<=rx_data; receive_left<=rx_data; req_data<=0; receive_index<=0;
                           receive_crc<=crc_byte(receive_crc,rx_data);
                           receive_state<=rx_data==0 ? 4 : 3;
                       end
                    3: begin
                           if (receive_index<37) req_data[295-receive_index*8 -: 8]<=rx_data;
                           receive_index<=receive_index+1'b1;
                           receive_crc<=crc_byte(receive_crc,rx_data);
                           receive_left<=receive_left-1'b1;
                           if (receive_left==1) receive_state<=4;
                       end
                    4: begin
                           if (rx_data==receive_crc && receive_address==8'h20) req_valid<=1;
                           receive_state<=0;
                       end
                    default: receive_state<=0;
                endcase
            end
            case (transmit_state)
                0: if (response_valid && response_length>=5 && response_length<=36) begin
                       transmit_payload<=response_data; transmit_index<=0;
                       transmit_length<=response_length;
                       transmit_crc<=8'hff; transmit_state<=1;
                   end
                1: if (!tx_busy) begin
                       tx_data<=next_tx; tx_start<=1;
                       if (transmit_index<transmit_length+3) transmit_crc<=crc_byte(transmit_crc,next_tx);
                       if (transmit_index>=3 && transmit_index<transmit_length+3)
                           transmit_payload<={transmit_payload[279:0],8'd0};
                       transmit_state<=2;
                   end
                2: if (tx_busy) transmit_state<=3;
                3: if (!tx_busy) begin
                       if (transmit_index==transmit_length+3) begin response_ack<=1; transmit_state<=4; end
                       else begin transmit_index<=transmit_index+1'b1; transmit_state<=1; end
                   end
                4: if (!response_valid) transmit_state<=0;
                default: transmit_state<=0;
            endcase
        end
    end
endmodule
