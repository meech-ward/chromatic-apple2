// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
module a2_audio_tb;
    reg clk=0;always #5 clk=~clk;
    reg reset=1,arm=0,esp_data=0;
    reg nack_all=0,corrupt_read=0,fail_power=0,stretch=0;
    wire esp_bclk,esp_ws,codec_mclk,codec_bclk,codec_ws,codec_data,codec_reset_n,ready;
    wire[7:0]flags,error_code,last_page,last_register,last_read;
    wire[71:0]readback;
    wire[15:0]din_ones,frame_count;
    tri1 scl,sda;
    a2_audio #(.I2C_HALF_TICKS(4),.STARTUP_TICKS(8),.SETTLE_TICKS(16),
               .I2C_TIMEOUT_TICKS(5000)) dut(.*);
    a2_codec_model model(.reset_n(codec_reset_n),.nack_all(nack_all),.corrupt_read(corrupt_read),
        .fail_power(fail_power),.stretch(stretch),.scl(scl),.sda(sda));
    integer cycles=0,last_mclk=0,last_bclk=0,last_ws=0,observed_ones=0;
    reg[30:0]sequence_bits=31'h6a51d35b;
    always @(posedge clk)begin
        cycles=cycles+1;
        if(reset)begin last_mclk=0;last_bclk=0;last_ws=0;observed_ones=0;end
    end
    always @(negedge esp_bclk)begin
        #1;
        if(!reset)begin
            sequence_bits={sequence_bits[29:0],sequence_bits[30]^sequence_bits[27]};
            esp_data=sequence_bits[0];
        end else esp_data=0;
    end
    always @(posedge codec_mclk)if(!reset)begin
        if(last_mclk!=0&&cycles-last_mclk!=4)$fatal(1,"MCLK is not board clock/4");
        last_mclk=cycles;
    end
    always @(posedge codec_bclk)begin
        #1;
        if(!reset)begin
            if(last_bclk!=0&&cycles-last_bclk!=32)$fatal(1,"BCLK is not board clock/32");
            if(esp_bclk!==codec_bclk||esp_ws!==codec_ws)$fatal(1,"ESP and codec clocks are not coherent");
            if(codec_data!==(ready&&dut.streaming ? esp_data : 1'b0))
                $fatal(1,"I2S relay shifted or corrupted a sampled bit");
            if(ready&&dut.streaming&&esp_data)observed_ones=observed_ones+1;
            last_bclk=cycles;
        end
    end
    always @(codec_ws)if(!reset)begin
        #1;
        if(codec_bclk!==0)$fatal(1,"WS changed away from BCLK falling edge");
        if(last_ws!=0&&cycles-last_ws!=512)$fatal(1,"WS half-frame is not16bits");
        last_ws=cycles;
    end
    always @(frame_count)if(!reset&&frame_count!=0)begin
        #1;
        if(codec_bclk!==0||codec_ws!==0)$fatal(1,"Frame counter changed away from coherent frame boundary");
    end
    task clocks(input integer count);repeat(count)begin @(posedge clk);#2;end endtask
    task await_ready;
        integer count;
        begin
            count=0;
            while(!ready&&error_code==0&&count<100000)begin clocks(1);count=count+1;end
            if(!ready||error_code!=0)$fatal(1,"Codec failed normal setup: error%0d page%0d reg%h read%h",error_code,last_page,last_register,last_read);
        end
    endtask
    task fail_setup(input[7:0]expected);
        integer count,before_unmute;
        begin
            before_unmute=model.unmute_writes;
            @(negedge clk);reset=1;arm=1;clocks(3);
            @(negedge clk);reset=0;count=0;
            while(error_code==0&&count<100000)begin clocks(1);count=count+1;end
            if(error_code!==expected||ready||codec_reset_n||codec_data||!flags[4]||flags[3])
                $fatal(1,"Failed codec setup was not silent/reset: expected%0d got%0d",expected,error_code);
            if(model.unmute_writes!=before_unmute)$fatal(1,"Failed verification unmuted a driver");
            clocks(100);
            if(ready||codec_reset_n||codec_data)$fatal(1,"Latched codec error unexpectedly retried/unmuted");
        end
    endtask
    integer before_ones,before_frames;
    initial begin
        clocks(4);@(negedge clk);reset=0;
        await_ready();
        if(readback!==72'h008182800094048690||last_page!=0||last_register!=8'h40||last_read!=4)
            $fatal(1,"Codec verification diagnostics mismatch: %h",readback);
        if(model.registers[8'h9f][7:6]!=0||model.registers[8'h41]!=8'hc4||
           model.registers[8'ha6]!=8'h92||model.registers[8'haa][4:3]!=0)
            $fatal(1,"Headphone-off/conservative fixed-gain settings incorrect");
        clocks(2048);
        if(codec_data||din_ones||frame_count)$fatal(1,"Unarmed audio was not silent");
        @(negedge clk);arm=1;
        wait(dut.streaming);clocks(1024*8);
        if(din_ones!==observed_ones[15:0]||din_ones<10||frame_count<7)
            $fatal(1,"Audio evidence counters incorrect ones%0d expected%0d frames%0d",din_ones,observed_ones,frame_count);
        wait(codec_ws&&codec_bclk);@(negedge clk);arm=0;clocks(1);
        if(!dut.streaming)$fatal(1,"Disarm cut a stereo frame mid-word");
        wait(!dut.streaming);clocks(1);before_ones=din_ones;before_frames=frame_count;
        clocks(2048);
        if(codec_data||din_ones!=before_ones||frame_count!=before_frames)
            $fatal(1,"Disarmed audio continued output/counter activity");
        nack_all=1;fail_setup(1);nack_all=0;
        corrupt_read=1;fail_setup(3);corrupt_read=0;
        fail_power=1;fail_setup(4);fail_power=0;
        stretch=1;fail_setup(2);stretch=0;
        @(negedge clk);reset=1;arm=0;clocks(3);
        if(codec_reset_n||codec_data||din_ones||frame_count)$fatal(1,"Audio reset did not return to silence");
        $display("PASS: codec ACK/readback, safe sequencing/failures, I2S clocks/relay, frame gating and evidence counters");$finish;
    end
    initial begin #10000000;$fatal(1,"Timeout");end
endmodule
