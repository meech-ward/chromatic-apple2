# Brick Out on the Chromatic

Our original Applesoft game follows the sideways Apple II Breakout described in
Apple's [1978 Reference Manual, pages 10–12](https://apple2online.com/wp-content/uploads/Red-Book.pdf)
and the [1977 Mini Manual listing](https://cmosher01.github.io/Apple-II-Source/breakout.html).
This is new game code, not a copy of Apple's program. The later Applesoft
**Little Brick Out** is credited to Bruce Tognazzini in
[Apple's 1979 tape manual, page 3](https://www.brutaldeluxe.fr/projects/cassettes/apple/apple_theappletapes_appleiiplus_manual.pdf).

## The familiar game

- Magenta field, blue paddle on the left, white ball and orange/yellow bricks.
- Eight columns of 20 bricks, worth 1–8 points per brick: **720** clears the wall.
- Five balls. Each starts near the center and travels toward the paddle.
- Six rebound angles: shallow near the paddle center, steep near its ends.
- Horizontal speed doubles after ten paddle returns, resetting for the next ball.
- Wall/paddle/brick clicks, the point scale under the columns and a final rating.

## Handheld controls

Hold **Up/Down** to move, press **A** to serve each ball or restart, and **B** to
pause/resume. **MENU** opens the regular machine menu. SSH supports Up/Down or
W/S, Space/Return to serve, and MENU as usual. **SELECT** or **Ctrl-C** stops
the BASIC program.

A separate serve button replaces the old timed start. Name/color questions and
the hidden automatic paddle are omitted so the whole game works without typing.
The classic board uses Woz's colors; the later Apple II Plus game's alternate
palette, high-score display and win bonus are not reproduced.

The BASIC source is [BREAKOUT.bas](../basic/BREAKOUT.bas). Its generated 6502
helper handles fast input, vertical paddle updates and clicks; the ball is drawn
before its previous position is erased. The helper reserves `$9000–$95FF` with
`HIMEM:36864`, uses six-byte entry stubs at `$0300`, and keeps state in
`$03C0–$03C8`, below DOS vectors. It does not use Applesoft zero page.
The game keeps one-third-row angle increments in Applesoft floating point rather
than the original Integer BASIC's scaled integer coordinates.

[Usage](usage.md) covers saving the source to your DOS disk.
[Verification](verification.md) distinguishes emulator checks from hardware tests.
