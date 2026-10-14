// SPDX-License-Identifier: MIT
// ROM reads, mapper controls and SRAM writes. ROM command cycles require an
// explicit build-time gate and a separately identified request type.
// CART_MODE is an explicit board choice: 0 disabled, 1 legacy, 2 switched power.
module a2_cartridge #(
    parameter CLOCK_HZ=33554432, parameter CART_MODE=0, parameter FLASH_WRITES=0,
    parameter POWER_TICKS=CLOCK_HZ/10,
    parameter RESET_TICKS=CLOCK_HZ/500,
    parameter SETTLE_TICKS=CLOCK_HZ/50,
    parameter SHUTDOWN_TICKS=CLOCK_HZ/1000,
    parameter SETUP_TICKS=8, parameter READ_TICKS=32, parameter WRITE_TICKS=16, parameter HOLD_TICKS=8
) (
    input wire clk, input wire reset, input wire switch_on,
    input wire present, input wire legacy_strap,
    output wire ready,
    input wire read_start, input wire [15:0] read_address,
    input wire write_start, input wire [7:0] write_value,
    input wire write_is_flash, input wire write_is_burst,
    output reg read_done, output reg read_failed, output reg [7:0] read_value,
    input wire [7:0] data_in,
    output reg [15:0] address,
    output wire power_enable, output wire control_enable,
    output wire reset_low, output wire cs_n, output wire rd_n,
    output wire wr_n, output wire data_input, output reg [7:0] data_out
);
    localparam OFF=0, POWER=1, RESET_CART=2, SETTLE=3, RUN=4, SHUTDOWN=5;
    reg [2:0] power_state=OFF;
    reg [31:0] power_timer=0;
    reg [1:0] present_sync=0, legacy_sync=0, switch_sync=0;
    wire revision_matches=(CART_MODE==1 && legacy_sync[1]) || (CART_MODE==2 && !legacy_sync[1]);
    wire requested=CART_MODE!=0 && revision_matches && present_sync[1] && switch_sync[1];
    assign ready=power_state==RUN && requested;
    assign power_enable=CART_MODE==2 && power_state!=OFF;
    assign control_enable=power_state==RESET_CART || power_state==SETTLE || power_state==RUN;
    assign reset_low=power_state==RESET_CART;
    reg select_ram=0, direction_out=0, write_strobe_n=1;
    assign cs_n=!(ready && select_ram);
    assign wr_n=ready ? write_strobe_n : 1'b1;
    // On an abort, retain driven data until the external isolator is disabled.
    // Normal writes release data only after their complete hold interval.
    assign data_input=!(control_enable && direction_out);
    reg [1:0] read_state=0;
    reg [7:0] read_timer=0;
    reg read_strobe_n=1;
    reg operation_write=0;
    assign rd_n=ready ? read_strobe_n : 1'b1;
    wire is_ram=read_address[15:13]==3'b101;
    wire readable=read_address<16'h8000 || is_ram;
    wire ordinary_write=is_ram || (read_address==16'h0000 && (write_value==0 || write_value==8'h0a)) ||
                  read_address==16'h2000 || (read_address==16'h3000 && write_value==0) ||
                  (read_address==16'h4000 && write_value<=3);
    wire flash_address=(read_address>=16'h4000 && read_address<16'h8000) ||
                       (read_address==0 && write_value==8'hf0) ||
                       (read_address==16'h00aa && write_value==8'h98) ||
                       (read_address==16'h0555 && write_value==8'h55) ||
                       (read_address==16'h0aaa && (write_value==8'haa || write_value==8'h90 ||
                                                 write_value==8'ha0 || write_value==8'h80));
    wire writable=write_is_burst ? (FLASH_WRITES!=0 && write_is_flash &&
                                   read_address>=16'h4000 && read_address<16'h8000) :
                  write_is_flash ? (FLASH_WRITES!=0 && flash_address) : ordinary_write;
    always @(posedge clk) begin
        if (reset) begin
            present_sync<=0; legacy_sync<=0; switch_sync<=0;
            power_state<=OFF; power_timer<=0;
        end else begin
            present_sync<={present_sync[0],present};
            legacy_sync<={legacy_sync[0],legacy_strap};
            switch_sync<={switch_sync[0],switch_on};
            if (power_state!=OFF && power_state!=SHUTDOWN && !requested) begin
                power_state<=SHUTDOWN; power_timer<=SHUTDOWN_TICKS;
            end else case (power_state)
                OFF: if (requested) begin power_state<=POWER; power_timer<=POWER_TICKS; end
                POWER: if (power_timer>1) power_timer<=power_timer-1'b1;
                       else begin power_state<=RESET_CART; power_timer<=RESET_TICKS; end
                RESET_CART: if (power_timer>1) power_timer<=power_timer-1'b1;
                            else begin power_state<=SETTLE; power_timer<=SETTLE_TICKS; end
                SETTLE: if (power_timer>1) power_timer<=power_timer-1'b1;
                        else power_state<=RUN;
                SHUTDOWN: if (power_timer>1) power_timer<=power_timer-1'b1;
                          else power_state<=OFF;
                RUN: ;
                default: power_state<=OFF;
            endcase
        end
    end
    // The external data bus is sampled only after a complete read interval.
    always @(posedge clk) begin
        if (reset) begin
            read_state<=0; read_timer<=0; read_strobe_n<=1; address<=0;
            read_done<=0; read_failed<=0; read_value<=0;
            operation_write<=0; select_ram<=0; direction_out<=0; write_strobe_n<=1; data_out<=0;
        end else begin
            read_done<=0;
            if (read_state!=0 && !ready) begin
                read_strobe_n<=1; read_state<=0; read_done<=1; read_failed<=1; read_value<=0;
                write_strobe_n<=1; direction_out<=0; select_ram<=0;
            end else case (read_state)
                0: begin
                    read_strobe_n<=1;
                    write_strobe_n<=1; direction_out<=0; select_ram<=0;
                    if (read_start || write_start) begin
                        read_failed<=!ready || (read_start && write_start) || (write_start ? !writable : !readable);
                        if (!ready || (read_start && write_start) || (write_start ? !writable : !readable)) begin
                            read_done<=1; read_value<=0;
                        end else begin
                            address<=read_address; operation_write<=write_start;
                            select_ram<=is_ram; direction_out<=write_start; data_out<=write_value;
                            read_timer<=SETUP_TICKS; read_state<=1;
                        end
                    end
                end
                1: if (read_timer>1) read_timer<=read_timer-1'b1;
                   else begin
                       if (operation_write) write_strobe_n<=0; else read_strobe_n<=0;
                       read_timer<=operation_write ? WRITE_TICKS : READ_TICKS; read_state<=2;
                   end
                2: if (read_timer>1) read_timer<=read_timer-1'b1;
                   else begin
                       read_value<=operation_write ? data_out : data_in;
                       read_strobe_n<=1; write_strobe_n<=1;
                       read_timer<=HOLD_TICKS; read_state<=3;
                   end
                3: if (read_timer>1) read_timer<=read_timer-1'b1;
                   else begin
                       read_done<=1; read_failed<=0; read_state<=0; direction_out<=0; select_ram<=0;
                   end
            endcase
        end
    end
endmodule
