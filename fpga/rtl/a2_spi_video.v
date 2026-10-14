// SPDX-License-Identifier: MIT
// 160x144 packed 4bpp framebuffer. SPI mode 0, at most 1 MHz with a
// 33.554432 MHz clk. A transaction is 01, byte_offset_hi, byte_offset_lo,
// followed by pixel bytes (left pixel in bits 7:4). CS ends any partial byte.
module a2_spi_video (
    input wire clk,
    input wire reset_n,
    input wire spi_sck,
    input wire spi_mosi,
    input wire spi_cs_n,
    input wire [14:0] pixel_address,
    output wire [3:0] pixel_data,
    output reg frame_ready
);
    localparam BYTE_COUNT = 11520;
    localparam PIXEL_COUNT = 23040;
    localparam COMMAND = 3'd0, OFFSET_HIGH = 3'd1, OFFSET_LOW = 3'd2,
               PAYLOAD = 3'd3, IGNORE = 3'd4;

    reg [7:0] pixels [0:BYTE_COUNT-1];
    integer i;
    initial begin
        for (i = 0; i < BYTE_COUNT; i = i + 1)
            pixels[i] = 8'h00;
    end

    // Independent synchronized read and write ports allow block RAM inference.
    reg [7:0] read_byte;
    reg read_low_nibble, read_valid;
    wire valid_address = pixel_address < PIXEL_COUNT;
    wire [13:0] read_address = valid_address ? pixel_address[14:1] : 14'd0;
    reg write_enable;
    reg [13:0] write_address;
    reg [7:0] write_data;
    always @(posedge clk) begin
        read_byte <= pixels[read_address];
        read_low_nibble <= pixel_address[0];
        read_valid <= reset_n && valid_address;
        if (reset_n && write_enable)
            pixels[write_address] <= write_data;
        if (!reset_n)
            frame_ready <= 1'b0;
        else if (write_enable)
            frame_ready <= 1'b1;
    end
    assign pixel_data = !read_valid ? 4'h0 :
                        read_low_nibble ? read_byte[3:0] : read_byte[7:4];

    // Sample external pins into clk. SPI inputs must remain stable around the
    // physical rising edge; mode 0 provides half a bit period for settling.
    (* async_reg = "true" *) reg [2:0] sck_sync;
    (* async_reg = "true" *) reg [1:0] mosi_sync, cs_sync;
    wire sck_rising = sck_sync[1] && !sck_sync[2];
    always @(posedge clk) begin
        if (!reset_n) begin
            sck_sync <= 3'b000;
            mosi_sync <= 2'b00;
            cs_sync <= 2'b11;
        end else begin
            sck_sync <= {sck_sync[1:0], spi_sck};
            mosi_sync <= {mosi_sync[0], spi_mosi};
            cs_sync <= {cs_sync[0], spi_cs_n};
        end
    end

    reg [2:0] state, bit_count;
    reg [7:0] shift, offset_high;
    reg [15:0] offset;
    wire [7:0] incoming = {shift[6:0], mosi_sync[1]};
    wire [15:0] incoming_offset = {offset_high, incoming};
    always @(posedge clk) begin
        write_enable <= 1'b0;
        if (!reset_n) begin
            state <= COMMAND;
            bit_count <= 3'd0;
            shift <= 8'd0;
            offset_high <= 8'd0;
            offset <= 16'd0;
            write_address <= 14'd0;
            write_data <= 8'd0;
        end else if (cs_sync[1]) begin
            state <= COMMAND;
            bit_count <= 3'd0;
            shift <= 8'd0;
        end else if (sck_rising) begin
            shift <= incoming;
            bit_count <= bit_count + 3'd1;
            if (bit_count == 3'd7) begin
                case (state)
                    COMMAND: state <= incoming == 8'h01 ? OFFSET_HIGH : IGNORE;
                    OFFSET_HIGH: begin
                        offset_high <= incoming;
                        state <= OFFSET_LOW;
                    end
                    OFFSET_LOW: begin
                        offset <= incoming_offset;
                        state <= incoming_offset < BYTE_COUNT ? PAYLOAD : IGNORE;
                    end
                    PAYLOAD: begin
                        if (offset < BYTE_COUNT) begin
                            write_address <= offset[13:0];
                            write_data <= incoming;
                            write_enable <= 1'b1;
                            offset <= offset + 16'd1;
                        end else begin
                            state <= IGNORE;
                        end
                    end
                    default: state <= IGNORE;
                endcase
            end
        end
    end
endmodule
