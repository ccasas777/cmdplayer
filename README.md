# cmdplay

`cmdplay` is a command-line audio player for Linux. It uses FFmpeg libraries
to decode audio files and ALSA for playback. It supports gapless playback,
CUE sheets, playlists, keyboard control, optional file preloading, and
configurable ALSA output devices.

## Features

- FLAC, APE, WAV, M4A, TTA, TAK, AIFF, DSF, DFF, MP3, Ogg, Opus, and other
  formats supported by the installed FFmpeg libraries
- CUE sheet track splitting, including single-file album images
- Selecting individual CUE tracks from a playlist
- Gapless playback when the output format permits it
- Seeking, pause, track selection, and playlist display from the keyboard
- Optional preloading of the current and next audio file
- Native output format or always-32-bit `S32_LE` output
- ALSA device selection and optional extra ALSA configuration

## Requirements

The project currently targets Linux and requires:

- GCC or another C11 compiler
- GNU Make
- `pkg-config`
- ALSA development files
- FFmpeg development files for `libavformat`, `libavcodec`, and `libavutil`

On Debian or Ubuntu, install the build dependencies with:

```bash
sudo apt install build-essential pkg-config libasound2-dev \
  libavformat-dev libavcodec-dev libavutil-dev
```

## Build

```bash
make
```

The executable is created as `./cmdplay`. To install it system-wide:

```bash
sudo make install
```

The default installation prefix is `/usr/local`. It can be changed:

```bash
make PREFIX="$HOME/.local" install
```

Remove build outputs with:

```bash
make clean
```

## Basic usage

Play one or more files:

```bash
./cmdplay song.flac
./cmdplay song1.flac song2.wav
```

Play a CUE sheet:

```bash
./cmdplay album.cue
```

Use a playlist:

```bash
./cmdplay --playlist list.txt
```

Print the expanded playlist without playing:

```bash
./cmdplay --playlist list.txt --list
```

Start at a particular item in the expanded playlist:

```bash
./cmdplay --playlist list.txt --track 3
```

Show all command-line options:

```bash
./cmdplay --help
```

## Playlist files

A playlist contains one path per line. Empty lines and lines beginning with
`#` are ignored. Relative paths are resolved relative to the playlist file.
Both regular audio files and `.m3u`/`.m3u8`-style playlists are accepted.

Example:

```text
/music/artist/album/album.cue
/music/artist/single.flac
/music/artist/live/track.wav
```

### Selecting CUE tracks

An unsuffixed CUE path expands to all audio tracks:

```text
/music/artist/album/album.cue
```

Append a colon-separated list of track numbers to select only those tracks:

```text
/music/artist/album/album.cue:1:2:7:12
```

This selects tracks 1, 2, 7, and 12. Track numbers that do not exist in the
CUE sheet are skipped. The order in the playlist entry is not used to reorder
tracks; selected tracks remain in their original CUE order.

The CUE path must end in `.cue` before the selection suffix. For example:

```text
album.cue:1:4
```

## Keyboard controls

Keyboard input is enabled when the program is connected to a terminal and
`keys = yes` is set in the configuration.

| Key | Action |
|---|---|
| `Space` | Pause or resume |
| `n` | Next track |
| `b` | Previous track |
| `Left` / `Right` | Seek backward/forward by `seek_step` seconds |
| `Down` / `Up` | Seek backward/forward by `seek_step_long` seconds |
| `g` | Enter a playlist track number to jump to |
| `l` | List tracks |
| `i` | Show current track information |
| `q` | Quit |

Keyboard input can be disabled with:

```bash
./cmdplay --no-keys song.flac
```

## Configuration

The default configuration file is:

```text
~/.config/cmdplay/cmdplay.conf
```

Use another configuration file with:

```bash
./cmdplay --settings /path/to/cmdplay.conf song.flac
```

The repository includes [cmdplay.conf](cmdplay.conf) as an example. Important
settings include:

```ini
[output]
device = default
sample_format = S32_LE
buffer_time = 500
period_time = 100
allow_resample = no

[player]
gapless = yes
pause_mode = reopen
seek_step = 10
seek_step_long = 60
keys = yes
text_encodings = BIG5,GB18030,SHIFT_JIS
preload = yes
preload_max = 4096
```

`text_encodings` controls the fallback encodings tried when a CUE sheet is not
UTF-8.

### ALSA configuration

The `alsa_conf` setting can point to an additional ALSA configuration file.
The repository includes [asound_cmdplay.conf](asound_cmdplay.conf) as an
example for the `alsa_cdsp` PCM device. Paths in this file are system-specific
and may need to be changed before use.

For a normal ALSA device, set for example:

```ini
[output]
device = default
alsa_conf =
```

The command-line `--device` option overrides the configured device:

```bash
./cmdplay --device hw:0,0 song.flac
```

## Source layout

| Path | Description |
|---|---|
| `src/` | C source code |
| `Makefile` | Build and installation rules |
| `cmdplay.conf` | Example player configuration |
| `asound_cmdplay.conf` | Example ALSA configuration |
| `patches/` | Local ALSA/cmdplay integration patches |
| `source/` | Audio measurement and correction data |
| `doc/` | Reference documents and frequency-response images |

Personal playlists and generated files under `tmp/` are excluded by
[.gitignore](.gitignore).

## License

No project license has been selected yet. Add a `LICENSE` file before
redistributing the project.
