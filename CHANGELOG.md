# Changelog

## [0.4.2] - 2026-09-29

### Fixed
- **按住说话（PTT）热键完全不工作**：松开事件按 key code 匹配，而修饰键的
  code 未必在松开时保留（部分前端给 0），导致松开分支被跳过——录音停不下来、
  状态一直停在「录音中」。改为按 keysym 匹配，并用 `isReleaseOfModifier`
  兜底修饰键的合成释放形态
- **松开一次后再按热键无任何反应**：`Pipeline::Start()` 在流水线运行中直接返回，
  而 PTT 松开恰恰只停采集、流水线仍在运行，于是采集再也不会重启
- **PTT 实际仍在走 Silero 分段**：`SetDirectPush()` 从未被生产代码调用；同时
  直推路径用「帧队列空闲」判断结束，而 32ms 帧间隔下每 2ms 轮询必然看到空闲，
  每段语音在第一帧后就被截断。现在模式由 Init 与配置热加载统一设置，
  结束改为松手后的显式请求
- **首次按下热键响应慢**：此前到按下才创建线程、加载 ONNX 模型、打开音频设备。
  新增 `Prepare()`，切换输入法时即预启动线程与引擎，按下只需开采集
- 引擎未配置时按热键无任何反馈（状态栏仍显示录音中），现在明确提示
- 松开后状态被 VAD 的 End 回调覆盖为「就绪」，现保持「识别中」直到结果返回
- `StopCapture()` 先请求收尾再停采集，会让 worker 在采集仍在产帧时提前收尾、
  丢掉尾音；改为先停采集（join）再请求收尾

### Changed
- `Pipeline::Start()` 返回 `bool`，失败可被调用方感知
- 移除 `pttHeldKeyCode_`（匹配改为 keysym 后成为只写不读的死状态）

## [0.4.1] - 2026-08-12

### Changed
- CI 重构：7 发行版容器矩阵构建（Ubuntu 24.04/26.04、Debian 12/13、Fedora 44、
  openSUSE Tumbleweed、Arch），产出 DEB/RPM/pkg.tar.zst 原生包（#22）
- onnxruntime 双策略：发行版系统包优先（DEB 开 dpkg-shlibdeps 自动依赖、
  RPM 由 rpmbuild 自动生成），无系统包的发行版下载 upstream release（1.28.0）
- AUR 构建去除 Docker daemon（archlinux 容器内 makepkg）
- release 改用 softprops/action-gh-release v3 创建 draft release

### Fixed
- 修复 curl >= 8.2.0 的 `curl_ws_recv` metap 参数 const 化导致的编译失败
  （Debian 12 curl 7.88 等老版本，按 LIBCURL_VERSION_NUM 条件分支）

### Added（本仓库）
- 小米 MiMo ASR：复用 OpenAI 兼容引擎，强制 chat 格式 + api-key 认证
- 按住说话（PTT）模式：按住热键录音，松开上屏，热键可配置
- 录音时状态栏音量电平条（10 格，200ms 刷新）
- 单元测试套件（`tests/`，CI 各发行版构建内跑 ctest）
- 配置界面后端选项按「协议/模式（典型提供商）」标注，API 模式写清传输方式
- AUR 构建配方（`aur/PKGBUILD`）与独立 RPM spec（`rpm/`），随 release 发布

### Fixed（本仓库）
- VAD 起始帧被重复送入 ASR，导致每段语音开头多出 32ms 音频
- PulseAudio 读取可能卡死时不再 join，避免退出流程冻结
- 队列加上容量上界，防止异常路径下内存无界增长
- 配置热加载竞态；PTT 短语音被静音阈值误丢弃
- PTT 结果重复上屏、热键二次按下失效
- 打包元数据：PKGBUILD 许可证与模型来源、RPM spec 版本与文件清单、
  DEB/RPM 的 URL 与描述均修正为指向本仓库

## [0.4.0] - 2026-08-11

### Added
- OpenAI GPT-Realtime 流式实时转录（#10/#11）
- 输入法图标 SVG + 多尺寸 PNG（#13）
- 录音后端可选依赖：PipeWire/PulseAudio 任一缺失时仅失去对应后端，addon 仍可构建（#19）
- 录音库运行期 dlopen 延迟加载：无链接期 DT_NEEDED 依赖，库升级/soname 变更不影响已安装 addon（#19）
- DEB 包补齐 Depends/Recommends 依赖声明；CI 新增 build-no-pipewire 降级构建验证（#19）

### Fixed
- 修复会话线程 UAF 与多处数据竞争（#14/#15）
- 日志降级、配置权限、停止路径与资源上限加固（#14/#16）
- Realtime 引擎周期 commit 失效 / End 尾部音频丢失 / preedit 回退（#12）
- 修复 PulseAudio Stop 跨线程 `pa_simple_free` 导致的 `free(): invalid pointer` 崩溃（#17）
- 修复 classicui 不显示图标问题（#13）

## [0.3.1] - 2026-07-14

### Changed
- 构建/安装脚本移至 `scripts/`

## [0.3.0] - 2026-07-13

### Added
- 输入法图标：SVG 矢量图标 + 多尺寸 PNG（16/22/24/32/48）
- `voiceinput.conf` 输入法注册配置文件
- 输入法注册采用 `OnDemand=True` conf 模式，对齐 fcitx5 官方做法

### Fixed
- 修复 classicui 不显示图标问题（仅 SVG 不被 GTK 图标主题识别）
- 修复 conf + C++ `listInputMethods()` 双重注册冲突
- 清理无意义的假多语言占位

## [0.2.0] - 2026-06-30

### Added
- 火山引擎 WebSocket ASR 支持
- LLM 后处理客户端（文本润色）
- ASR Session Reaper（会话回收机制）

## [0.1.5] - 2026-06-15

### Added
- PulseAudio 音频捕获后端
- Silero VAD 集成
- 管道编排（FrameQueue → VADWorker → UtteranceQueue → ASRWorker）

## [0.1.0] - 2026-06-01

### Added
- 初始版本：OpenAI 兼容 API ASR
- PipeWire 音频捕获
- Fcitx5 InputMethodEngineV2 集成
