# Clean Mic

Real-time microphone noise suppression and voice gating for Linux using PipeWire, LADSPA, and RNNoise.

Clean Mic captures audio from your hardware microphone, removes background noise in real time (both while speaking and while silent), and gates to silence when no voice is present. It creates a virtual PipeWire microphone node named **Clean Mic** that any application can use.

## Installation

Run the installation script from the repository root:

```bash
./install.sh
```

This will:
1. Compile the `rnnoise_gate` LADSPA filter plugin to `~/.local/lib/cleanmic/librnnoise_gate_ladspa.so`.
2. Install the PipeWire filter-chain configuration template to `~/.config/cleanmic/clean-mic.conf.in`.
3. Install the CLI management tool `cleanmic` to `~/.local/bin/cleanmic`.
4. Install the GTK4/Libadwaita GUI `cleanmic-gui` to `~/.local/bin/cleanmic-gui` and register its desktop launcher.
5. Install the systemd user service `clean-mic.service` to `~/.config/systemd/user/`.
6. Regenerate `~/.config/cleanmic/clean-mic.conf` from your settings and, if the service is already running, restart it so changes take effect (it never starts or enables the service by itself).

## Using "Clean Mic" in Meet, OBS, and Other Apps

Once Clean Mic is running:
1. Open your meeting or recording app (Google Meet, OBS Studio, Discord, Zoom, etc.).
2. Go to the audio settings and select **Clean Mic** as your input microphone instead of your physical device.
3. In Google Meet or Discord, you can disable their built-in noise cancellation to prevent unnecessary double-processing.

## Graphical Interface (GUI)

Launch **Clean Mic** from your application launcher or terminal:

```bash
cleanmic-gui
```

The GUI allows you to:
- Toggle **Noise suppression** on and off (controls the background service).
- Select the physical **Microphone** input source.
- Tune **Suppression strength** (wet/dry mix).
- Adjust **Voice sensitivity** (VAD probability threshold to open the gate).
- Configure **Gate release** (hold duration before closing the gate).
- Set **Gate floor** (gain applied during silence; default -80 dB).
- Toggle **Use Clean Mic as default microphone** to make `clean_mic` the system default input (turning it off restores your real microphone).
- Toggle **Start with login** to enable or disable the systemd user service.

## Command-Line Interface (CLI)

The `cleanmic` CLI provides full management and automation capabilities:

```bash
# Check service and virtual microphone status (JSON output)
cleanmic status

# List available physical microphones
cleanmic sources
cleanmic sources --detailed

# Service control
cleanmic start      # Start the Clean Mic service
cleanmic stop       # Stop the Clean Mic service
cleanmic restart    # Restart the Clean Mic service
cleanmic enable     # Enable Clean Mic service on login
cleanmic disable    # Disable Clean Mic service on login

# View and update configuration
cleanmic get
cleanmic get vad_threshold
cleanmic set source "alsa_input.usb-Fifine_Microphone-00.mono-fallback"
cleanmic set suppression 1.0
cleanmic set vad_threshold 0.6
cleanmic set release_ms 200
cleanmic set floor_db -80   # default; higher (e.g. -60) lets more noise through in silence
cleanmic apply      # Re-generate configuration and apply changes

# Default microphone
cleanmic default on      # Make Clean Mic the system default input (starts the service if needed)
cleanmic default off     # Restore the previous real microphone as default
cleanmic default status  # Print on/off (add --json for details)
```

## Uninstallation

To stop the service and remove all installed files:

```bash
./uninstall.sh
```
