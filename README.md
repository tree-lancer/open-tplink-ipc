# tplink-ipc-mcp

简体中文 | [English](README-en.md)

面向中国大陆 TP-LINK TL-IPC 摄像机的 C++ MCP 服务，通过局域网连接摄像机，无需 TP-LINK 云服务。目前支持并验证的设备是 **TL-IPC48AW-PLUS V5.0**（固件 `1.0.3 Build 20260119`）；其他型号可能需要适配。

## 能做什么

| 功能 | MCP 工具 |
|---|---|
| 查看激活状态、设备信息与运行状态 | `camera_activation_status`、`camera_info`、`camera_status` |
| 截图、数字裁剪、录制片段 | `camera_snapshot`、`camera_record_clip` |
| 云台移动与预置位管理 | `camera_ptz_status`、`camera_move`、`camera_go_to_position`、`camera_presets`、`camera_save_preset`、`camera_go_to_preset`、`camera_remove_preset` |
| 侦测与设备设置 | `camera_set_detection`、`camera_set_tracking`、`camera_set_privacy`、`camera_set_night_vision`、`camera_set_led`、`camera_set_speaker_volume` |
| 监听移动、遮挡和人形白光报警 | `camera_events`、`camera_person_alarm` |
| 播放音频与手动声光报警 | `camera_play_audio`、`camera_alarm`、`camera_stop_alarm` |

## 下载

在 [GitHub Releases](https://github.com/tree-lancer/open-tplink-ipc/releases/latest) 下载 `tplink-ipc-mcp-linux-x86_64`，适用于 x86_64 Linux。下载后赋予执行权限：

```sh
chmod +x tplink-ipc-mcp-linux-x86_64
```

同一页面提供 `SHA256SUMS`，可用 `sha256sum -c SHA256SUMS` 校验下载文件。推送 `v` 开头的版本标签后，GitHub Actions 会自动构建并发布该二进制。其他平台可按下文自行构建。

## 本地构建

需要 C++17、CMake、libcurl、OpenSSL、libxml2 和 nlohmann/json 的开发包。截图、录像与播放音频还需要 `ffmpeg`。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 使用

先在 TP-LINK App 或摄像机网页完成首次激活并设置密码。然后在终端运行下载的二进制（本地构建则使用 `build/tplink-ipc-mcp`）：

```sh
./tplink-ipc-mcp-linux-x86_64 configure
./tplink-ipc-mcp-linux-x86_64 check
```

`configure` 会询问局域网 IP、用户名（默认 `admin`）和密码。密码输入不回显；程序验证登录成功后，保存到当前用户的 `~/.config/tplink-ipc-mcp/config.json`，文件权限为 `0600`。`check` 显示连接是否就绪。换设备或改密码时重新运行 `configure`。

在 MCP 客户端中添加一个 **stdio** 服务，把命令设为二进制的绝对路径，无需附加参数或旧项目脚本。例如 Codex 的 `~/.codex/config.toml`：

```toml
[mcp_servers.tplink_ipc]
command = "/absolute/path/tplink-ipc-mcp-linux-x86_64"
```

如需不落盘配置，可用 `IPC_HOST`、`IPC_USER`、`IPC_PASSWORD` 环境变量；它们也会覆盖已保存的对应字段。`IPC_MEDIA_DIR` 可指定录像目录，`IPC_AUDIO_DIR` 可指定播放音频的目录。

客户端连接后即可按工具名调用。`camera_snapshot` 默认返回 640×360 截图，`high_quality=true` 返回 4K；`zoom=2` 至 `5` 可数字裁剪，`focus_x` 和 `focus_y`（0–1）指定裁剪区域的位置。`camera_record_clip` 接受 1–20 秒的时长并返回本地 MKV 文件路径。`camera_play_audio` 接受 `IPC_AUDIO_DIR` 中的 `.wav`、`.mp3`、`.m4a` 或 `.ogg` 文件名。

## 使用注意

- `camera_person_alarm` 最多等待 30 秒。它临时启用人形白光联动、关闭普通移动白光联动，检测到闪灯状态后返回 `detected=true`，并恢复原设置。结果来自联动状态；若同时手动触发白光报警，可能产生相同信号。工具不会主动播放声音，且会拒绝已有声音联动的配置。
- `camera_activation_status` 只需相机地址，可确认是否需要首次激活。已激活设备的原密码无法从局域网接口读取；请在 `configure` 中输入你先前设置的密码。
- 配置文件保存可供本机用户读取的明文密码，请保护操作系统账户，不要将该文件提交到仓库或共享目录。
- `camera_events` 提供普通移动和遮挡事件，不提供人形分类。`camera_person_alarm` 是当前获取人形触发信号的方法。
- `camera_alarm` 会发出声音并闪灯；仅在明确需要现场报警时调用。`camera_stop_alarm` 可立即停止手动报警。
- `camera_record_clip` 保存到运行 MCP 的电脑，无需摄像机安装 SD 卡。`camera_play_audio` 的文件上限为 10 MiB，播放上限为 30 秒。
- Release 二进制无需安装 C++ 运行库、libcurl、OpenSSL 或 libxml2；截图、录像和播放音频仍需在运行 MCP 的电脑上安装 `ffmpeg` 并确保它在 `PATH` 中。
- 摄像机的 HTTP 控制与 RTSP 连接在局域网内使用明文，请在可信网络中使用。

协议与型号兼容性见[协议文档](docs/protocol.md)。

## 许可证

MIT，详见 [LICENSE](LICENSE)。
