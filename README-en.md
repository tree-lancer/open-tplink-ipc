# tplink-ipc-mcp

[简体中文](README.md) | English

A C++ MCP server for mainland China TP-LINK TL-IPC cameras. It connects over the local network without TP-LINK cloud services. The currently supported and verified device is **TL-IPC48AW-PLUS V5.0** with firmware `1.0.3 Build 20260119`; other models may need adaptation.

## Features

| Capability | MCP tools |
|---|---|
| Activation, device information, and status | `camera_activation_status`, `camera_info`, `camera_status` |
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

First activate the camera and set its password in the TP-LINK app or camera web page. Then run the downloaded binary in a terminal (or use `build/tplink-ipc-mcp` for a local build):

```sh
./tplink-ipc-mcp-linux-x86_64 configure
./tplink-ipc-mcp-linux-x86_64 check
```

`configure` prompts for the camera's LAN address, username (default `admin`), and password without echoing the password. It verifies the login before saving `~/.config/tplink-ipc-mcp/config.json` with mode `0600`. Run it again when the camera or password changes. `check` confirms that the saved connection works.

Add a **stdio** server to your MCP client with the binary's absolute path as its command. No extra arguments or old project scripts are needed. For example, in Codex's `~/.codex/config.toml`:

```toml
[mcp_servers.tplink_ipc]
command = "/absolute/path/tplink-ipc-mcp-linux-x86_64"
```

For setups without a config file, `IPC_HOST`, `IPC_USER`, and `IPC_PASSWORD` can be passed as environment variables; they also override matching saved fields. `IPC_MEDIA_DIR` optionally selects a recording directory, and `IPC_AUDIO_DIR` selects an audio input directory.

After the client connects, call tools by name. `camera_snapshot` returns a 640×360 image by default; `high_quality=true` uses 4K. Set `zoom` to 2–5 for a digital crop and `focus_x`/`focus_y` (0–1) to position the crop. `camera_record_clip` records 1–20 seconds and returns the local MKV path. `camera_play_audio` accepts a `.wav`, `.mp3`, `.m4a`, or `.ogg` basename from `IPC_AUDIO_DIR`.

## Important notes

- `camera_person_alarm` waits for up to 30 seconds. It temporarily enables the person white-light linkage, disables the ordinary motion light linkage, returns `detected=true` when the light alarm activates, and restores the original settings. Its result is inferred from the linkage state; a concurrent manual light alarm can produce the same signal. The tool does not play sound and refuses configurations with active sound linkages.
- `camera_activation_status` only needs a camera address and reports whether initial activation is needed. An activated camera does not disclose its existing password over the LAN API; enter the password you set earlier in `configure`.
- The config file stores a plaintext password accessible to your local OS account. Keep it out of repositories and shared folders.
- `camera_events` reports ordinary motion and tamper events without person classification. Use `camera_person_alarm` for the available person-triggered signal.
- `camera_alarm` makes audible noise and flashes the light. Call it only when an on-site alarm is intended. `camera_stop_alarm` stops a manual alarm immediately.
- `camera_record_clip` saves to the computer running MCP and does not require a camera SD card. `camera_play_audio` accepts files up to 10 MiB and plays for at most 30 seconds.
- The release binary needs no separate C++ runtime, libcurl, OpenSSL, or libxml2 installation. Snapshots, recording, and audio playback still require `ffmpeg` installed on the MCP host and available in `PATH`.
- The camera's HTTP control and RTSP traffic are unencrypted on the LAN. Use a trusted network.

See the [protocol documentation](docs/protocol.md) for compatibility details.

## License

MIT. See [LICENSE](LICENSE).
