## Russian Internet Radio Player (fmplay.ru)

A lightweight internet radio player written in C using ncurses and mpv.
Plays stations with 24kbps/s aac-he-v2 bitrate

## Screenshot

![Radio Player](https://github.com/nallion/FMPLAY-linux/blob/main/Screenshot%202026-11-24%2011.01.28.png)

## Requirements

- GCC
- ncursesw
- mpv
- ALSA

## Compilation

```bash
sudo apt install build-essential libncurses-dev mpv
gcc -D_DEFAULT_SOURCE -O2 -Wall -Wextra radio.c -o radio -lncursesw
```
or run compile.sh (after installing dependencies)

Project written with ChatGPT
