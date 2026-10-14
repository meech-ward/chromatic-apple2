// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_spi_video_tb;
    reg clk = 1'b0;
    always #15 clk = ~clk;
    reg reset_n = 1'b0, spi_sck = 1'b0, spi_mosi = 1'b0, spi_cs_n = 1'b1;
    reg [14:0] pixel_address = 15'd0;
    wire [3:0] pixel_data;
    wire frame_ready;
    a2_spi_video dut (
        .clk(clk), .reset_n(reset_n), .spi_sck(spi_sck),
        .spi_mosi(spi_mosi), .spi_cs_n(spi_cs_n),
        .pixel_address(pixel_address), .pixel_data(pixel_data), .frame_ready(frame_ready)
    );

    task begin_transfer;
        begin
            @(negedge clk);
            spi_sck = 0;
            spi_cs_n = 0;
            #200;
        end
    endtask
    task end_transfer;
        begin
            spi_sck = 0;
            #200;
            spi_cs_n = 1;
            #200;
        end
    endtask
    task send_byte;
        input [7:0] value;
        integer bit_index;
        begin
            for (bit_index = 7; bit_index >= 0; bit_index = bit_index - 1) begin
                spi_sck = 0;
                spi_mosi = value[bit_index];
                #500;
                spi_sck = 1;
                #500;
            end
            spi_sck = 0;
        end
    endtask
    task header;
        input [15:0] byte_offset;
        begin
            send_byte(8'h01);
            send_byte(byte_offset[15:8]);
            send_byte(byte_offset[7:0]);
        end
    endtask
    task read_pixel;
        input [14:0] address;
        input [3:0] expected;
        reg [3:0] previous;
        begin
            @(negedge clk);
            previous = pixel_data;
            pixel_address = address;
            #1;
            if (pixel_data !== previous)
                $fatal(1, "read output changed before its clock edge");
            @(posedge clk);
            #1;
            if (pixel_data !== expected)
                $fatal(1, "pixel %0d expected %h, got %h", address, expected, pixel_data);
        end
    endtask

    integer index;
    initial begin
        repeat (5) @(posedge clk);
        @(negedge clk);
        reset_n = 1;
        // Every initial pixel must be black, including the final partial RAM block.
        for (index = 0; index < 23040; index = index + 1)
            read_pixel(index, 4'h0);
        if (frame_ready !== 0) $fatal(1, "ready before first payload write");

        begin_transfer;
        header(16'd0);
        if (frame_ready !== 0) $fatal(1, "header set ready");
        send_byte(8'h12);
        end_transfer;
        if (frame_ready !== 1) $fatal(1, "first payload did not set ready");
        read_pixel(0, 4'h1);
        read_pixel(1, 4'h2);

        begin_transfer;
        header(16'd3);
        send_byte(8'hab);
        send_byte(8'hcd);
        end_transfer;
        read_pixel(4, 0);
        read_pixel(5, 0);
        read_pixel(6, 4'ha);
        read_pixel(7, 4'hb);
        read_pixel(8, 4'hc);
        read_pixel(9, 4'hd);
        read_pixel(10, 0);

        // An invalid command ignores the whole transaction, including 01 bytes.
        begin_transfer;
        send_byte(8'h02);
        header(16'd0);
        send_byte(8'hff);
        end_transfer;
        read_pixel(0, 4'h1);
        read_pixel(1, 4'h2);

        // CS discards a partial header and a partially received payload byte.
        begin_transfer;
        send_byte(8'h01);
        send_byte(8'h00);
        end_transfer;
        begin_transfer;
        header(16'd0);
        repeat (4) begin
            spi_mosi = 1;
            #500; spi_sck = 1;
            #500; spi_sck = 0;
        end
        end_transfer;
        read_pixel(0, 4'h1);
        read_pixel(1, 4'h2);
        begin_transfer;
        header(16'd5);
        send_byte(8'h34);
        end_transfer;
        read_pixel(10, 4'h3);
        read_pixel(11, 4'h4);

        // Last valid byte is writable; subsequent data must never wrap to zero.
        begin_transfer;
        header(16'd11519);
        send_byte(8'hfe);
        send_byte(8'h99);
        send_byte(8'h88);
        end_transfer;
        read_pixel(23038, 4'hf);
        read_pixel(23039, 4'he);
        read_pixel(0, 4'h1);
        begin_transfer;
        header(16'd11520);
        send_byte(8'h77);
        end_transfer;
        begin_transfer;
        header(16'hffff);
        send_byte(8'h66);
        end_transfer;
        read_pixel(0, 4'h1);
        read_pixel(23038, 4'hf);
        read_pixel(23040, 0);
        read_pixel(32767, 0);

        // Exercise the independent read port during a streaming SPI write.
        fork
            begin
                begin_transfer;
                header(16'd100);
                for (index = 0; index < 16; index = index + 1)
                    send_byte(index);
                end_transfer;
            end
            begin
                repeat (3000) begin
                    read_pixel(0, 4'h1);
                    read_pixel(1, 4'h2);
                end
            end
        join
        for (index = 0; index < 16; index = index + 1) begin
            read_pixel(200 + index * 2, 0);
            read_pixel(201 + index * 2, index);
        end

        // A logic reset clears ready/parser, while configured RAM retains pixels.
        @(negedge clk); reset_n = 0;
        repeat (4) @(posedge clk);
        #1;
        if (frame_ready !== 0 || pixel_data !== 0) $fatal(1, "reset outputs");
        @(negedge clk); reset_n = 1;
        read_pixel(0, 4'h1);
        begin_transfer;
        header(16'd0);
        send_byte(8'h56);
        end_transfer;
        read_pixel(0, 4'h5);
        read_pixel(1, 4'h6);
        if (frame_ready !== 1) $fatal(1, "ready after reset/write");
        $display("PASS a2_spi_video: initialization, mode0, offsets, nibble order, CS truncation, rejection, concurrent reads");
        $finish;
    end
    initial begin
        #10000000;
        $fatal(1, "test timeout");
    end
endmodule
