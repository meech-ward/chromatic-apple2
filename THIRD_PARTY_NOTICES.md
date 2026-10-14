# Third-party software and inputs

The project's original emulator integration, FPGA controller, tools and Breakout
program are MIT licensed. [LICENSE](LICENSE) applies to that code. Included
third-party files retain their own notices and licenses:

| Component | License | Notices and provenance |
| --- | --- | --- |
| MII 65C02 CPU core, via frank-apple | MIT | [License](mcu/licenses/MII-MIT.txt), [source manifest](mcu/SOURCE-MANIFEST.json) |
| Disk II sector codec ancestry, including Apple2JS | MIT | [License](mcu/licenses/Apple2JS-MIT.txt), [source manifest](mcu/SOURCE-MANIFEST.json) |
| LittleFS | BSD-3-Clause | [License](storage/vendor/littlefs/LICENSE.md), [pinned sources](storage/vendor/littlefs/UPSTREAM.json) |
| staticnet SSH cryptography and adapted transport | BSD-3-Clause | [License](mcu/main/ssh/vendor/staticnet/LICENSE), [adaptation notes](mcu/main/ssh/vendor/UPSTREAM.md) |
| TweetNaCl and libb64 subset | Public domain, as stated upstream | [Adaptation notes](mcu/main/ssh/vendor/UPSTREAM.md) and retained source headers |

ESP-IDF and the FPGA build tools are external dependencies. Their own component
and tool licenses remain applicable; they are not vendored in this repository.

Apple ROMs, DOS disk images, commercial programs, factory firmware backups and
provisioned firmware binaries are **not included**. Supply your own inputs that
you are entitled to use. The project license does not grant rights to those
external inputs. In particular, firmware built with your ROMs and network
credentials is a private build artifact, not a source-release attachment.

No ModRetro or MiSTer FPGA implementation or proprietary Gowin IP is included.
This is an independent homebrew project, not an official ModRetro or Apple
product.
