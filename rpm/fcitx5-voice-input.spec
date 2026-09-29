# Silero VAD 模型来自 git 子模块，而 GitHub 标签归档不含子模块内容，
# 因此单独作为 Source1 提供（固定 commit → URL 不可变，可校验）
%global silero_vad_commit dbacf536adadf42210f37ae50fbaf75f6235b3cf

Name:           fcitx5-voice-input
Version:        0.4.2
Release:        1%{?dist}
Summary:        Fcitx5 voice input addon with OpenAI-compatible and Volcengine Doubao ASR
License:        LGPL-3.0-or-later
URL:            https://github.com/WenYin-Community/fcitx5-voice-input
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz
Source1:        https://raw.githubusercontent.com/snakers4/silero-vad/%{silero_vad_commit}/src/silero_vad/data/silero_vad.onnx

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(Fcitx5Core)
BuildRequires:  pkgconfig(jsoncpp)
BuildRequires:  pkgconfig(libcurl) >= 7.86.0
BuildRequires:  pkgconfig(zlib)
# onnxruntime 未提供 pkgconfig 虚拟名，用实际包名（与 CI distro 脚本一致）
BuildRequires:  onnxruntime-devel
BuildRequires:  pkgconfig(libpulse-simple)
BuildRequires:  pkgconfig(libpulse)
BuildRequires:  pkgconfig(libpipewire-0.3)
%if 0%{?suse_version}
BuildRequires:  gettext-runtime
%else
BuildRequires:  gettext
%endif

# 采集后端在运行期 dlopen（无 SONAME 依赖，不会自动生成 Requires），
# 至少需要安装其中一个。按 soname 声明可跨发行版成立：Fedora 的
# pulseaudio-libs/pipewire-libs 与 openSUSE 的 libpulse0/libpipewire-0_3-0
# 名称不同，但都提供这两个 soname
Recommends:     libpulse.so.0
Recommends:     libpipewire-0.3.so.0

%description
fcitx5-voice-input is a Fcitx5 addon for voice input. It captures audio via
PulseAudio (or PipeWire fallback), detects speech segments with Silero ONNX
VAD, and transcribes via OpenAI-compatible API (OpenAI Whisper, Groq,
SiliconFlow, Xiaomi MiMo ASR, etc.).

%prep
%autosetup -n %{name}-%{version}
# 补齐标签归档缺失的子模块内容（CMake 在模型缺失时直接报错）
install -Dm644 %{SOURCE1} third_party/silero-vad/src/silero_vad/data/silero_vad.onnx

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DBUILD_TESTS=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_libdir}/fcitx5/voice-input-addon.so
%{_datadir}/fcitx5/addon/voiceinput.conf
%{_datadir}/fcitx5/inputmethod/voiceinput.conf
%{_datadir}/fcitx5/voice-input/models/silero_vad.onnx
%{_datadir}/icons/hicolor/scalable/apps/fcitx_voiceinput.svg
%{_datadir}/icons/hicolor/*/apps/fcitx_voiceinput.png
%{_datadir}/locale/*/LC_MESSAGES/fcitx5-voice-input.mo

%changelog
* Tue Sep 29 2026 Wenyin Root <ruojiner@hotmail.com> - 0.4.2-1
- 修复按住说话（PTT）热键：松开事件按 keysym 匹配（此前按 key code，
  修饰键松开时的 code 不保证保留，导致录音停不下来）
- 修复松开后再按热键无反应（流水线运行中 Start 直接返回）
- 修复 PTT 未启用直推模式、且按帧队列空闲判断结束导致语音被截断
- 新增 Prepare()：切换输入法时预启动线程与引擎，消除首次按下的延迟
- 引擎未配置时按热键给出明确提示，不再谎报录音中

* Tue Sep 29 2026 Wenyin Root <ruojiner@hotmail.com> - 0.4.1-1
- 仓库迁移至 WenYin-Community，更新 URL 与维护者
- 规格修正：%files 与实际安装布局对齐（补输入法配置与 PNG 图标）
- 采集后端改为 Recommends（运行期 dlopen，不再硬依赖 pipewire-libs +
  pulseaudio-libs 两者）
- 源码改为标签归档 + 固定 commit 的 Silero VAD 模型（归档不含子模块）

* Wed Jul 01 2026 Wenyin Root <64475363+devcxl@users.noreply.github.com> - 0.1.4-1
- Fix PTT: text appeared 3 times due to duplicate commitString in OnAsrResult
- Fix PTT: second hotkey press not working, Pipeline::Start() now handles restart

* Wed Jul 01 2026 Wenyin Root <64475363+devcxl@users.noreply.github.com> - 0.1.3-1
- PTT release delayed stop (200ms) to capture trailing audio
- Audio level visualization during recording (RMS-based bar indicator)
- Stop audio capture immediately on PTT release (StopCapture)

* Wed Jul 01 2026 Wenyin Root <64475363+devcxl@users.noreply.github.com> - 0.1.2-1
- Fix PTT: commit ASR result directly from worker thread (bypass eventDispatcher)
- Add tray icon via InputMethodEntry::setIcon()
- Remove non-standard Icon field from addon conf
- Remove debug keyEvent logging

* Wed Jul 01 2026 Wenyin Root <64475363+devcxl@users.noreply.github.com> - 0.1.1-1
- Fix PTT mode: ASR result not committed after hotkey release
- Commit text directly from ASR callback to avoid generation race condition

* Wed Jul 01 2026 Wenyin Root <64475363+devcxl@users.noreply.github.com> - 0.1.0-1
- Initial RPM package
- PulseAudio / PipeWire audio capture
- Silero ONNX VAD speech segmentation
- OpenAI-compatible ASR backend
- Xiaomi MiMo ASR backend
- LLM post-processing support
- fcitx5-configtool graphical configuration
