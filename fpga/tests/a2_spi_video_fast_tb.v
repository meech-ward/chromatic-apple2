// SPDX-License-Identifier: MIT
`timescale 1ps/1ps
// Candidate-link simulation only. Production remains configured for 1 MHz.
module a2_spi_video_fast_tb;
    localparam real CORE_HALF_PS=500000000000.0/33554432;
    localparam real SPI_HALF_PS=125000.0; // 4 MHz mode 0.
    localparam integer BYTES=11520;
    reg clk=0;
    always #(CORE_HALF_PS) clk=~clk;
    reg reset_n=0,spi_sck=0,spi_mosi=0,spi_cs_n=1;
    reg [14:0] pixel_address=0;
    wire [3:0] pixel_data;
    wire frame_ready;
    a2_spi_video dut(.*);

    reg [7:0] model[0:BYTES-1],expected[0:BYTES-1];
    reg transaction_active=0,manual_read=0;
    integer next_write=0,write_end=0,received=0,read_checks=0;
    reg [3:0] read_expected;
    always @(negedge clk) begin
        if(!manual_read)
            pixel_address=pixel_address+97<BYTES*2 ? pixel_address+97 : pixel_address+97-BYTES*2;
    end
    always @(posedge clk) begin
        if(reset_n) begin
            // Infer the documented read-first RAM result before applying the
            // simultaneous write to our independent framebuffer model.
            read_expected=pixel_address[0] ? model[pixel_address/2][3:0] : model[pixel_address/2][7:4];
            if(dut.write_enable) begin
                if(!transaction_active || next_write>=write_end || dut.write_address!==next_write)
                    $fatal(1,"Missing/duplicate/out-of-range byte: expected address %0d got %0d",next_write,dut.write_address);
                if(dut.write_data!==expected[next_write])
                    $fatal(1,"Corrupt byte %0d: expected %02x got %02x",next_write,expected[next_write],dut.write_data);
                model[next_write]=expected[next_write];
                next_write=next_write+1;
                received=received+1;
            end
            #1;
            if(pixel_data!==read_expected)
                $fatal(1,"Concurrent read mismatch at pixel %0d",pixel_address);
            read_checks=read_checks+1;
        end
    end

    function [7:0] pattern(input integer address,seed);
        reg [31:0] x;
        begin
            x=address*32'h45d9f3b ^ seed*32'h9e3779b9;
            x=x^(x>>13);x=x*32'h27d4eb2d;x=x^(x>>15);
            pattern=x[7:0];
        end
    endfunction
    function [31:0] crc_byte(input [31:0] previous,input [7:0] value);
        reg [31:0] c;integer bit_index;
        begin
            c=previous^value;
            for(bit_index=0;bit_index<8;bit_index=bit_index+1)
                c=c[0] ? (c>>1)^32'hedb88320 : c>>1;
            crc_byte=c;
        end
    endfunction
    task send_byte(input [7:0] value);
        integer bit_index;
        begin
            for(bit_index=7;bit_index>=0;bit_index=bit_index-1) begin
                spi_sck=0;spi_mosi=value[bit_index];
                #(SPI_HALF_PS);spi_sck=1;
                #(SPI_HALF_PS);
            end
            spi_sck=0;
        end
    endtask
    task transfer(input integer offset,count,seed,input real phase_ps,gap_ps);
        integer index;
        begin
            for(index=offset;index<offset+count;index=index+1)
                expected[index]=pattern(index,seed);
            @(negedge clk);#(phase_ps);
            next_write=offset;write_end=offset+count;transaction_active=1;
            spi_cs_n=0;
            send_byte(8'h01);send_byte(offset>>8);send_byte(offset);
            // ESP sends header and DMA pixels as separate transfers under CS.
            #(gap_ps);
            for(index=offset;index<offset+count;index=index+1)
                send_byte(expected[index]);
            #(SPI_HALF_PS*2);spi_cs_n=1;
            repeat(5) @(negedge clk);
            transaction_active=0;
            if(next_write!=write_end)
                $fatal(1,"Incomplete receive: expected end %0d got %0d",write_end,next_write);
        end
    endtask
    task verify_frame;
        integer index;
        reg [3:0] high_nibble;
        reg [31:0] observed_crc,expected_crc;
        begin
            @(posedge clk);#2;manual_read=1;
            observed_crc=32'hffffffff;expected_crc=32'hffffffff;
            for(index=0;index<BYTES;index=index+1) begin
                @(negedge clk);pixel_address=index*2;
                @(posedge clk);#2;high_nibble=pixel_data;
                @(negedge clk);pixel_address=index*2+1;
                @(posedge clk);#2;
                observed_crc=crc_byte(observed_crc,{high_nibble,pixel_data});
                expected_crc=crc_byte(expected_crc,expected[index]);
            end
            if(observed_crc!==expected_crc)
                $fatal(1,"Full-frame read-port CRC mismatch: expected %08x got %08x",~expected_crc,~observed_crc);
            if(frame_ready!==1) $fatal(1,"Full frame did not assert ready");
            manual_read=0;
        end
    endtask

    integer index,phase;
    initial begin
        for(index=0;index<BYTES;index=index+1) begin model[index]=0;expected[index]=0;end
        repeat(5) @(negedge clk);
        reset_n=1;
        // Four full frames sweep quarter-clock launch phases, include DMA gaps,
        // compare every received byte, and read back all 23,040 pixel nibbles.
        for(phase=0;phase<4;phase=phase+1) begin
            transfer(0,BYTES,phase+1,phase*CORE_HALF_PS/2,phase*1000000.0);
            verify_frame;
        end
        // Finer phase sweep exercises header/CS synchronization at 32 offsets
        // across one real 33.554432 MHz core period, with varying idle gaps.
        for(phase=0;phase<32;phase=phase+1)
            transfer(phase*256,256,phase+11,phase*CORE_HALF_PS/16,phase*100000.0);
        $display("PASS: 4MHz mode0, real 33.554432MHz core, 4 full-frame CRCs, 32 launch phases, %0d received bytes, %0d concurrent read checks",received,read_checks);
        $finish;
    end
    initial begin #2000000000000;$fatal(1,"Fast SPI test timeout");end
endmodule
