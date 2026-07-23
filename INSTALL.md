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

Stop and restart playback after changing the audio output backend.

## Notes

- The station list is saved automatically for your Windows user profile.
- If no stations are found, check your antenna, signal coverage, and SDRplay driver installation.
