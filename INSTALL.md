# DAB SDR GUI Installation and First Use

## Before You Start

To use the GUI with SDRplay hardware on Windows, install the SDRplay drivers and runtime first.

Download and install them from the official SDRplay website:

- https://www.sdrplay.com/downloads/

After the SDRplay software is installed, connect your SDRplay device.

## Running the Application

1. Start `dab_gui.exe`.
2. Press the `Scan` button to find available stations.
3. Select a station from the list.
4. Press the `Start` button to begin listening.

## Audio Output

Open `Settings` > `Audio Settings...` to select the audio output backend:

- `Automatic` prefers native Windows Audio (WASAPI) and falls back to PortAudio.
- `Windows Audio (WASAPI)` uses the default Windows multimedia output device.
- `PortAudio` uses the bundled PortAudio runtime.

Enable `Realtime decoding priority (Windows MMCSS)` if decoding is interrupted by other
CPU-intensive applications. It prioritizes only the DAB decoder and audio writer threads;
it does not use Windows' unsafe whole-process Realtime priority.

Stop and restart playback after changing an audio setting.

## Scan Timing

Open `Settings` > `Scan Settings...` to adjust:

- `Channel sample time` controls how much IQ data is checked on each DAB channel.
- `Extra multiplex label time` controls how long station metadata is collected after a multiplex is found.
- `Maximum channel time` is an emergency wall-clock limit for one channel.

The defaults are 4 seconds, 6 seconds, and 45 seconds respectively. Longer values can help with weak signals or slower computers, but make a complete Band III scan take longer.

## Notes

- The station list is saved automatically for your Windows user profile.
- If no stations are found, check your antenna, signal coverage, and SDRplay driver installation.
