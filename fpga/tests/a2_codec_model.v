// SPDX-License-Identifier: MIT
// Behavioral I2C codec register model for electrical/protocol tests only.
module a2_codec_model (
    input wire reset_n, input wire nack_all, input wire corrupt_read,
    input wire fail_power, input wire stretch,
    inout wire scl, inout wire sda
);
    reg drive_sda=0;
    assign sda=drive_sda ? 1'b0 : 1'bz;
    assign scl=stretch ? 1'b0 : 1'bz;
    reg [7:0] registers[0:255];
    reg active=0,ack_pending=0,ack_active=0,sending=0,send_nack=0,send_after_ack=0;
    reg ack_value=0;
    reg [7:0] shift=0,page=0,pointer=0,send_value=0;
    integer kind=0,bits=0,send_bit=0,starts=0,stops=0,writes=0,reads=0,unmute_writes=0,i;
    task clear_registers;
        begin
            for(i=0;i<256;i=i+1)registers[i]=0;
            registers[8'h40]=8'h0c;
            registers[8'h9f]=8'h04;
            registers[8'ha0]=8'h06;
            page=0;
        end
    endtask
    initial clear_registers();
    always @(negedge reset_n)begin
        clear_registers();active=0;drive_sda=0;ack_pending=0;ack_active=0;sending=0;send_nack=0;
    end
    always @(negedge sda)if(scl===1'b1&&reset_n)begin
        active=1;kind=0;bits=0;shift=0;ack_pending=0;ack_active=0;
        sending=0;send_nack=0;send_after_ack=0;drive_sda=0;starts=starts+1;
    end
    always @(posedge sda)if(scl===1'b1&&reset_n)begin
        active=0;drive_sda=0;stops=stops+1;
    end
    always @(posedge scl)if(active&&reset_n)begin
        if(ack_active)begin end
        else if(sending)begin
            if(send_nack)begin
                if(sda!==1'b1)$fatal(1,"Master failed to NACK its final read byte");
                sending=0;active=0;
            end
        end else begin
            shift={shift[6:0],sda};bits=bits+1;
            if(bits==8)begin
                bits=0;ack_pending=1;ack_value=!nack_all;send_after_ack=0;
                case(kind)
                    0: begin
                        if(shift[7:1]!==7'h18)$fatal(1,"Master addressed a device other than codec0x18");
                        if(shift[0])begin
                            send_after_ack=1;reads=reads+1;
                            if(page==0&&pointer==8'h25)
                                send_value=fail_power ? 0 :
                                    {registers[8'h3f][7],2'd0,registers[8'ha0][7],4'd0};
                            else send_value=registers[{page[0],pointer[6:0]}];
                            if(page==0&&pointer==8'h0b&&corrupt_read)send_value=send_value^1;
                        end else kind=1;
                    end
                    1: begin pointer=shift;kind=2;end
                    2: if(!nack_all)begin
                        if((page==1&&pointer==8'h2a&&shift[2])||
                           (page==0&&pointer==8'h40&&!shift[3]))unmute_writes=unmute_writes+1;
                        if(pointer==0)begin
                            if(shift>1)$fatal(1,"Codec setup touched an unexpected register page");
                            page=shift;
                        end else if(page==0&&pointer==1&&shift==1)clear_registers();
                        else registers[{page[0],pointer[6:0]}]=shift;
                        writes=writes+1;
                    end
                endcase
            end
        end
    end
    always @(negedge scl)if(active&&reset_n)begin
        if(ack_pending)begin
            drive_sda=ack_value;ack_pending=0;ack_active=1;
        end else if(ack_active)begin
            drive_sda=0;ack_active=0;
            if(!ack_value)active=0;
            else if(send_after_ack)begin
                sending=1;send_nack=0;send_bit=7;drive_sda=!send_value[7];
            end
        end else if(sending)begin
            if(send_bit==0)begin drive_sda=0;send_nack=1;end
            else begin send_bit=send_bit-1;drive_sda=!send_value[send_bit];end
        end
    end
endmodule
