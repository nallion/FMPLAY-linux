## Russian Internet Radio Player (fmplay.ru)

A lightweight very stable and reliable console internet radio player written in C using ncurses and mpv.
Plays stations with 24kbps/s aac-he-v2 bitrate.

## Screenshots
- Debian
![Radio Player](https://github.com/nallion/FMPLAY-linux/blob/main/Screenshot%202026-11-24%2011.01.28.png)
- MacOS
![Radio Player](https://github.com/nallion/FMPLAY-linux/blob/main/photo_2026-11-24_22-48-05.jpg)

## Requirements

- GCC
- ncursesw
- mpv
- ALSA

## Compilation & Running (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt install wget build-essential libncurses-dev mpv
wget https://raw.githubusercontent.com/nallion/FMPLAY-linux/refs/heads/main/radio.c
gcc -D_DEFAULT_SOURCE -O2 -Wall -Wextra radio.c -o radio -lncursesw
./radio
```

## Downloading & Running builded binary (Debian / Ubuntu)

```bash
sudo apt-get update
sudo apt install wget libncursesw6 mpv
wget "https://github.com/nallion/FMPLAY-linux/releases/download/Latest-1/radio"
chmod 777 radio
chmod +X radio
./radio
```

Project written with ChatGPT
