# TL-IPC48AW-PLUS V5.0 协议与兼容性

已验证 **至少** 适用于 TL-IPC48AW-PLUS V5.0、固件 `1.0.3 Build 20260119 Rel.38255n` 使用的局域网接口。

## 连接与认证

| 端口 | 用途 | 认证 |
|---|---|---|
| TCP 80 | 摄像机配置和状态的 JSON 接口 | 摄像机账号、RSA 加密登录、`stok` 会话令牌 |
| TCP 554 | RTSP 视频、音频和对讲 | RTSP Digest |
| TCP 2020 | ONVIF 设备信息、媒体、云台和事件 | WS-Security UsernameToken |

JSON 接口的登录流程：

1. `GET /pc/Content.htm` 获取随机数及 RSA 公钥。该型号公钥的 DER 整数编码不规范，客户端需容忍其编码方式。
2. 对密码执行设备网页端的密码变换，拼接随机数，使用 RSA PKCS#1 v1.5 加密，以加密类型 2 提交至 `POST /`。
3. 使用返回的 `stok` 调用 `POST /stok=<stok>/ds`。会话失效后重新登录。

`/ds` 请求采用 JSON，包含 `method`（`get`、`set` 或 `do`）及模块名称。接口字段和允许值依固件而定。MCP 工具负责构造请求、检查设备错误码和保护会话令牌。

## 功能与接口对应

| 功能 | 本地接口 |
|---|---|
| 设备信息与能力 | `device_info.basic_info`、`function.module_spec` |
| 人形、移动、遮挡侦测 | `people_detection.detection`、`motion_detection.motion_det`、`tamper_detection.tamper_det` |
| 人形与移动的声光联动 | `people_detection.notify_list`、`motion_detection.motion_notif_list` |
| 白光灯运行状态 | `image.light_status` |
| 目标跟踪与隐私遮蔽 | `target_track.target_track_info`、`lens_mask.lens_mask_info` |
| 夜视、状态灯、扬声器 | `image.switch`、`led.config`、`audio_config.speaker` |
| 手动报警 | `msg_alarm.manual_msg_alarm` |
| 云台预置位 | `preset.preset` 及相应操作 |
| 存储状态 | `harddisk_manage.hd_info` |

`function.module_spec` 是设备能力声明，不代表所有字段均可用于远程控制，工具仅使用本文列出的已适配接口。

## 人形报警信号

该固件通过 `people_detection` 进行设备端人形侦测。~~由于作者没能找到人形侦测相关事件~~，`camera_person_alarm` 使用人形白光联动作为局域网信号：

1. 确认人形侦测已启用，且已知的其他声光联动不会干扰判断；
2. 在调用窗口内启用人形白光联动，关闭普通移动白光联动；
3. 读取 `image.light_status.wl_status`。`blink` 表示白光报警处于闪烁状态；检测到后返回 `detected=true` 和 Unix 时间戳；
4. 无论正常结束还是发生请求错误，都尝试恢复调用前的人形与移动白光联动设置。

该工具最多等待 30 秒，不会主动启动声音报警。它识别的是**已隔离的人形白光联动状态**，不是带人形类别的原始事件；同时发生的手动白光报警可能产生相同状态。

ONVIF PullPoint 仅提供 `RuleEngine/CellMotionDetector/Motion` 和 `TamperDetector/Tamper` 主题。`camera_events` 返回这些主题及其 `SimpleItem` 字段；普通移动主题不含人形类别。该型号未提供可供本项目读取人形类别的 ONVIF 分析模块或元数据轨道。

## 视频、对讲与云台

| 通道 | 内容 |
|---|---|
| RTSP `stream1` | H.265，3840×2160，G.711 A-law 音频 |
| RTSP `stream2` | H.264，640×360，G.711 A-law 音频 |

`camera_snapshot` 从 RTSP 提取单帧，数字裁剪在本机完成；不依赖 ONVIF 快照地址。`camera_record_clip` 将 RTSP 流封装成 MKV，不转码，也不依赖摄像机 SD 卡。`camera_play_audio` 通过 RTSP `MULTITRANS` 会话发送 G.711 A-law 音频。

云台操作使用 ONVIF `profile_1`，PTZ SOAP 请求需要 `SOAPAction` 请求头。云台支持水平与垂直方向的位置和移动；数字裁剪不改变云台位置。该型号的 ONVIF PTZ 能力未提供电动变焦轴。

## 实现边界

- 本项目目前只保证上述设备与固件版本的兼容性。
- 人形报警工具依赖白光联动，调用期间摄像机会闪灯。并发手动白光报警可能造成误判。
- ONVIF 移动事件、白光联动状态和设备端人形分类是不同信号；调用方应按工具返回的信号类型处理。
- 摄像机内录像的检索与回放不属于当前 MCP 工具集。`camera_record_clip` 保存的是 MCP 所在电脑上的文件。
- HTTP 控制和 RTSP 连接未使用 TLS，部署环境应限制局域网访问，并安全保存摄像机账号。

## 相关资料

- [TL-IPC48AW Plus 产品页](https://www.tp-link.com.cn/product_3279.html?v=spe)
- [TL-IPC48AW Plus V5.0 固件下载页](https://resource.tp-link.com.cn/pc/docCenter/showDoc?id=1770732603076554)
- [TP-LINK RTSP 通道说明](https://media.tp-link.com.cn/document/html1715219996593.html)
- [MCP stdio 传输规范](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
