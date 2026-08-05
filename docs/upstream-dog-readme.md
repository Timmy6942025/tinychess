# Original Dog README (upstream, archived)

The upstream Dog README content is kept here for reference — original build
instructions, hardware pinouts and tips that are still useful for this fork.
Upstream: https://github.com/folkertvanheusden/Dog

---

## Building the original

To build the program and upload it to a wemos32 mini:

	cd app
	idf.py build && idf.py flash

The ESP32 version can be used with xboard as well (a serial wrapper was
provided upstream; this fork uses `tools/board_session.py` instead).

The program also has an integrated text-interface. For that just run it and
enter "tui".

To build it for Linux (requires at least gcc/g++ 14 or clang/clang++ 14,
gcc produces faster binaries):

	cd app/src/linux-windows
	mkdir build
	cd build
	cmake ..
	make

'Dog-native' is probably the fastest on your computer. If it won't run, try
Dog-avx512 then Dog-avx2 and if all fails, try Dog.

Debian/Ubuntu users can then also run:

    cpack

to get an installable .deb package-file.

To build it for windows (using mingw-w64):

	cd app/src/linux-windows
	mkdir buildwindows
	cd buildwindows
	cmake -DCMAKE_TOOLCHAIN_FILE=../mingw64.cmake ..
	make

The Linux/windows versions contain a Dog in ansi-art visible when you run it
with '-h'.

## LED pins (original board)

The device can have (optional) LEDs connected:
* a green led on pin 27 - blinks while thinking
* a blue led on pin 25  - blinks while pondering
* a red led on pin 22   - blinks in an error situation

You can also can connect a TTL to UART converter to pin 16 (RX) and pin 17 (TX).

* internal led is blinking during startup

Note: this fork targets the XIAO ESP32-S3 Plus (WS2812 LED on GPIO44, see
`app/src/Kconfig`) — the pins above are for the original wemos32 mini.

## Training help

`helping-me-out.md` (in this folder) documents how the upstream project
collects training data — relevant to the trainer pipeline in `RESEARCH.md`.
