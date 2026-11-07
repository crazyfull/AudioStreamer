# Examples

## loopback (`example/loopback`)

Command line program for libRavAva: microphone -> Opus -> (simulated network) -> jitter buffer -> speaker, on the same
machine. Fixed format: **48000 Hz, mono, fullband**. No Qt.

    cmake -S example/loopback -B build-loopback -DCMAKE_BUILD_TYPE=Release
    cmake --build build-loopback
    ./build-loopback/ravava_loopback --list
    ./build-loopback/ravava_loopback -b 64000 -f 20 --loss 5

libRavAva is built from `src/ravava` by default (static, miniaudio inside). `-DRAVAVA_USE_INSTALLED=ON` uses an installed copy
instead (`-DCMAKE_PREFIX_PATH=<prefix>`). `-DRAVAVA_WITH_WEBRTC=ON` (or AUTO when the library is installed) enables
`--echo / --ns / --agc`. Use headphones, otherwise the speaker howls into the microphone (unless `--echo` is on).

Options: `--list`, `-i/--in N`, `-o/--out N` (numbers from `--list`), `-b/--bitrate`, `-f/--frame`, `--music`, `--dtx`,
`--no-fec`, `--loss PCT`, `--reorder PCT` (simulated network), `-g/--gain`, `-v/--volume`, `--echo 0..4`, `--ns 0..4`, `--agc`, `--hpf` (low-cut filter, off by default),
`-t/--time SEC`, `-q/--quiet`. While it runs it shows the microphone (sent) and speaker (received) level and the receive statistics.
