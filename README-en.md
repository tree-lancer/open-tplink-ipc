# tplink-ipc-mcp

[简体中文](README.md) | English

A C++ MCP server for mainland China TP-LINK TL-IPC cameras. It connects over the local network without TP-LINK cloud services. The currently supported and verified device is **TL-IPC48AW-PLUS V5.0** with firmware `1.0.3 Build 20260119`; other models may need adaptation.

## Features

| Capability | MCP tools |
|---|---|
| Device information and status | `camera_info`, `camera_status` |
| Snapshots, digital crops, and clips | `camera_snapshot`, `camera_record_clip` |
| Pan, tilt, and presets | `camera_ptz_status`, `camera_move`, `camera_go_to_position`, `camera_presets`, `camera_save_preset`, `camera_go_to_preset`, `camera_remove_preset` |
| Detection and device settings | `camera_set_detection`, `camera_set_tracking`, `camera_set_privacy`, `camera_set_night_vision`, `camera_set_led`, `camera_set_speaker_volume` |
| Motion, tamper, and person-light alarm monitoring | `camera_events`, `camera_person_alarm` |
| Audio playback and manual sound/light alarm | `camera_play_audio`, `camera_alarm`, `camera_stop_alarm` |

## Download

Download `tplink-ipc-mcp-linux-x86_64` from [GitHub Releases](https://github.com/tree-lancer/open-tplink-ipc/releases/latest) for x86_64 Linux, then make it executable:

```sh
chmod +x tplink-ipc-mcp-linux-x86_64
```

The release also includes `SHA256SUMS`; run `sha256sum -c SHA256SUMS` to verify the download. Pushing a version tag beginning with `v` automatically builds and publishes the binary through GitHub Actions. Build locally for other platforms.

## Build locally

Requires C++17, CMake, and development packages for libcurl, OpenSSL, libxml2, and nlohmann/json. Snapshots, recording, and audio playback also require `ffmpeg`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Use

Add a **stdio** server to your MCP client. Set its command to the absolute path of the downloaded `tplink-ipc-mcp-linux-x86_64` or locally built `build/tplink-ipc-mcp`, and pass these environment variables:

| Variable | Purpose |
|---|---|
| `IPC_HOST` | Camera LAN IP address or hostname, without a port |
| `IPC_USER`, `IPC_PASSWORD` | Camera username and password |
| `IPC_MEDIA_DIR` | Optional existing directory for `camera_record_clip` output |
| `IPC_AUDIO_DIR` | Optional existing directory containing files for `camera_play_audio` |

For example, set the connection variables before starting your MCP client:

```sh
export IPC_HOST=192.168.1.100
export IPC_USER=admin
read -rs IPC_PASSWORD && export IPC_PASSWORD
```

After the client connects, call tools by name. `camera_snapshot` returns a 640×360 image by default; `high_quality=true` uses 4K. Set `zoom` to 2–5 for a digital crop and `focus_x`/`focus_y` (0–1) to position the crop. `camera_record_clip` records 1–20 seconds and returns the local MKV path. `camera_play_audio` accepts a `.wav`, `.mp3`, `.m4a`, or `.ogg` basename from `IPC_AUDIO_DIR`.

## Important notes

- `camera_person_alarm` waits for up to 30 seconds. It temporarily enables the person white-light linkage, disables the ordinary motion light linkage, returns `detected=true` when the light alarm activates, and restores the original settings. Its result is inferred from the linkage state; a concurrent manual light alarm can produce the same signal. The tool does not play sound and refuses configurations with active sound linkages.
- `camera_events` reports ordinary motion and tamper events without person classification. Use `camera_person_alarm` for the available person-triggered signal.
- `camera_alarm` makes audible noise and flashes the light. Call it only when an on-site alarm is intended. `camera_stop_alarm` stops a manual alarm immediately.
- `camera_record_clip` saves to the computer running MCP and does not require a camera SD card. `camera_play_audio` accepts files up to 10 MiB and plays for at most 30 seconds.
- The release binary needs no separate C++ runtime, libcurl, OpenSSL, or libxml2 installation. Snapshots, recording, and audio playback still require `ffmpeg` installed on the MCP host and available in `PATH`.
- The camera's HTTP control and RTSP traffic are unencrypted on the LAN. Use a trusted network and your client's secure storage for credentials; do not commit passwords to the repository.

See the [protocol documentation](docs/protocol.md) for compatibility details.

## License

MIT. See [LICENSE](LICENSE).
