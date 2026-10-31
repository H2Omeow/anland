# 桌面：在一个 anland 窗口里跑嵌套合成器

> 中文版（本文件）· English: [desktop.md](desktop.md)

anland 6 给每个 Wayland toplevel 一个独立的 Android 窗口。对单应用容器来说这很理想，
而这恰好也是桌面合成器**嵌套**运行时需要的东西：KWin、Weston、Sway、Mutter 本来就会
当另一个合成器的 Wayland 客户端（`--wayland-display`、`--backend=wayland`、
`WLR_BACKENDS=wayland`），而它们连上的宿主就是 anland。

所以“在手机上跑 KDE”不需要 KWin 后端插件（那是 5.x 的设计：`backend-anland` 每出一个
KWin 版本就要重写一遍，KWin 一改后端 API 就坏）。它需要一个“行为足够像合成器、能让
客户端合成器信任”的宿主。本文讲的就是这个宿主侧，加上启动嵌套会话的启动器。

```
  Android Activity（全屏 Surface）
        ▲  eglSwapBuffers / SurfaceControl
        │
  waylandbridge  ── wl_compositor, wl_shm, linux-dmabuf(v3, LINEAR),
        │            xdg_wm_base, wl_seat, wl_output(v4), zxdg_output_v1,
        │            wp_presentation, wl_subcompositor, wl_data_device
        │
        │  一个 toplevel = 整个桌面
        ▼
  KWin / Weston / Sway  （嵌套后端：GBM + dmabuf，可退回 shm）
        │
        ▼
  客户端应用（Wayland 的，以及通过合成器自带 Xwayland 的 X11 应用）
```

## 宿主对外提供的协议

| 协议 | 嵌套合成器为什么需要它 |
| --- | --- |
| `wl_compositor`、`wl_shm` | 客户端侧 buffer；GBM/EGL 导入不了 dmabuf 时唯一的路 |
| `zwp_linux_dmabuf_v1` v3 | GPU buffer，附带它能从中分配的 format + modifier 列表（本次构建：`ARGB8888`/`XRGB8888` × `LINEAR`、`INVALID`） |
| `xdg_wm_base`、`wl_subcompositor` | toplevel 角色、popup、subsurface（桌面外壳、tooltip） |
| `wl_seat` | 键盘（XKB keymap + modifiers）、指针（motion/buttons/axis/frame）、触摸 |
| `wl_output` v4 | 虚拟屏幕：geometry、mode、scale、**name** 与 **description** |
| `zxdg_output_manager_v1` v3 | 逻辑位置/尺寸 + 输出名，给 Qt/KWin/GTK 的输出模型用 |
| `wp_presentation` v1 | 真实的“第 N 帧在 T 时刻呈现、刷新率 R、序号 S”反馈 |
| `wl_data_device_manager` | 客户端应用之间的剪贴板与拖放 |
| `zwp_linux_explicit_synchronization_v1` | acquire/release fence，给偏好它而不是 frame callback 的客户端 |

`wl_output` 在设计上只增不减（`awl_output_grow`）：它从不缩小，所以一个曾经以
2400×1080 显示过的桌面，即使之后窗口显示得更小，也仍然保持那个分辨率。Xwayland 的
X 屏幕和大多数输出模型都期望如此。

### presentation-time 与 “mailbox” 问题

合成器靠两个信号给自己定节奏：frame callback（开始下一帧），以及（如果有的话）
presentation feedback（你刚提交的那帧现在已经上屏）。只有 frame callback 时，
合成器只能假设“回调来了 ⇒ 上一帧落地了”，而只要宿主的呈现路径是异步的，这个假设就是
错的——KDE 的 mailbox 提交模型于是会排进已经过时的帧。

`wp_presentation` 正是补这个缺口：`surface_presented()`（渲染线程，`eglSwapBuffers`
之后立刻）用 swap 的 `CLOCK_MONOTONIC` 时间戳、输出的刷新周期和一个递增序号，回答该
窗口所有排队的 feedback 对象。窗口死掉时，它的 feedback 对象回答 `discarded`，这样
合成器会拆掉自己的循环，而不是永远等下去。

时间戳是 swap 返回的时刻，不是面板的 vsync：anland 渲染进一个 Activity Surface，而这
条路上拿不到 `EGL_ANDROID_get_frame_timestamps`（`SurfaceControl` 从不产生 frame id）。
把它当成“此刻呈现 ± 一帧”即可——足够定节奏，不够做音画同步测量。

## 打开它

这两个键也在手机上：**窗口设置 → 桌面**——会话开关，以及标题列表的文本框（走 binder
应用、写回 `config.json`、在匹配窗口的下一次 configure 生效）。要在第一次开机前设置，
仍然可以改文件：

```json
{
  "sc_enabled": 0,
  "desktop_mode": 1,
  "desktop_match": "kwin,plasma,weston,sway,mutter,gnome-shell,wayfire"
}
```

路径是 `/data/adb/modules/anland-awl/config.json`，改完重启守护进程（或者走 binder 的
`cfg_set`/`cfg_set_str` 路径——`desktop_mode` 立即生效，`desktop_match` 会重新判定
已经开着的窗口）。

| 键 | 范围 | 含义 |
| --- | --- | --- |
| `desktop_mode` | 0–1 | 1 = 标题命中 `desktop_match` 的 toplevel 是整屏会话 |
| `desktop_match` | 字符串 | 逗号分隔、不区分大小写的标题子串 |

`sc_enabled` 必须保持 `0`：SurfaceControl 后端把每个窗口当成独立图层交给
SurfaceFlinger，守护进程根本不碰那些像素。只有 GL 后端才往 Activity Surface 里渲染，
也才是嵌套合成器那唯一一个窗口能被合成、被定节奏、（比如在 VR 里）被重投影的后端。

窗口命中 `desktop_mode` 之后会发生什么：

* 它的每一次 `xdg_toplevel.configure` 都额外带上 `FULLSCREEN | MAXIMIZED`
  （见 `awl_xdg.c`），于是合成器不再把宿主当成浮动窗口管理器；
* 窗口被钉到面板尺寸（`awl_window_resize` 到物理面板），`wl_output` 的 mode 也随之
  长大到覆盖它；
* 承载它的 Activity 被要求保持屏幕常亮——锁屏会掐死客户端的帧循环。

## 运行它

在容器里，从 anland 会话中：

```sh
bash anlandx/anland-session.sh &        # Xwayland + mini-wm + D-Bus，~/.anlandx-env
bash anlandx/anland-desktop             # 自动挑 kwin_wayland > weston > sway > mutter
bash anlandx/anland-desktop --compositor weston    # 显式指定
bash anlandx/anland-desktop --print-env           # 只打印会话环境
```

`anland-desktop` 会 source `~/.anlandx-env`，给嵌套会话一个独立的
`XDG_RUNTIME_DIR`（`$XDG_RUNTIME_DIR/anland-desktop`）和独立的 Wayland socket
（`wayland-desktop`），并写出 `~/.anlanddesk-env`，方便在桌面里启动应用：

```sh
. ~/.anlanddesk-env
wayland-info | head                 # 确认宿主提供了哪些全局对象
kate &                              # 嵌套桌面上的一个 Qt 应用
```

容器里装合成器（以 Arch 为例，括号里是 Debian/Ubuntu 的包名）：

```sh
pacman -S weston                    # ubuntu: weston            （最小，用来冒烟）
pacman -S sway                      # ubuntu: sway              （wlroots）
pacman -S kwin plasma-workspace     # ubuntu: kwin-wayland, plasma-workspace
```

Weston 是合适的第一测：它除了 EGL/GLES 什么都不需要，并且会明确告诉你走的是宿主的
dmabuf 路径还是 shm 路径。KWin 才是目标体验；它额外需要 Qt 6、KF6 和一个会话 D-Bus。

### 如果 GPU 导入失败

这里的 `zwp_linux_dmabuf_v1` 只广告 `LINEAR`，而宿主必须先把每个客户端 dmabuf 变成
`AHardwareBuffer`，EGL 才能采样（`awl_ahb.cpp`）。在某些 Adreno/Turnip 组合上，送来的
仍然是 tiled buffer，导入就会失败；这时合成器会自己退回 `wl_shm`
（KWin：`KWIN_COMPOSE=Q`；Sway：`WLR_RENDERER=pixman`；Weston：`--use-pixman`）。
这样能跑，但每帧都是一次 CPU 拷贝——用来诊断可以，日常用太慢。

## 验证一次会话

```sh
# 1. 守护进程把这个窗口认成了会话
logcat -s waylandbridge | grep -i 'desktop session'
#    window 7: desktop session on (Plasma Desktop), pinned to 1832x1920

# 2. 合成器期待的宿主全局对象
. ~/.anlanddesk-env
wayland-info | grep -E 'wl_output|zxdg_output|wp_presentation|linux_dmabuf'
#    wp_presentation (v1), zxdg_output_manager_v1 (v3), zwp_linux_dmabuf_v1 (v3)

# 3. 输出几何跟窗口一致
wayland-info | grep -A3 'interface: zxdg_output_v1'
#    logical_size: 1832x1920      <- 不是 800x600，也不是旧的窗口尺寸

# 4. presented 反馈在流动（KWin: KWIN_DEBUG=1，weston: WAYLAND_DEBUG=1）
```

时间戳合理的 `wp_presentation` `presented` 事件，意味着渲染循环是由宿主定节奏的，
而不是靠猜。关闭时出现的 `discarded` 事件是正常的。

## 已知限制

* **没有 DRM/KMS，没有直接扫描输出。** 桌面只是客户端的一个窗口；宿主
  （SurfaceFlinger）永远是最终合成者。客户端里的任何东西都无法直接上屏。
* **一次只能有一个桌面。** 会话标记是按窗口的，而面板只有一块：第二个匹配的窗口也会
  被钉住，但只有一个能看见。
* **只有 `LINEAR`。** tiled/压缩的客户端 buffer 必须由宿主导入；导入失败时，合成器会
  退回 `wl_shm`。
* **客户端拿不到硬件光标平面。** 合成器把光标画进自己的 buffer（普通窗口有宿主侧的
  光标图层）。
* **定节奏在宿主侧。** 只有在窗口的 buffer 到达、面板 vblank 时宿主才呈现；想要严格
  节奏的合成器应该遵守 `wp_presentation`，而不是假设固定刷新率。
* **VR 里仍然以 3DoF 为主。** 在 VR 会话里跑桌面，继承 `docs/vr.md` 的限制：默认只有
  朝向追踪（外部 6DoF 提供者可通过 `anland-xr-pose-ext` 接管）、只有一个 XR 窗口能被
  当作头显视图。
* **尚未真机验证。** 宿主协议、会话模式和启动器都能编译、也自洽；嵌套的 KWin/Plasma
  会话还没有在作者手上的硬件跑过。上面的验证步骤就是在真机上该照着做的，
  `logcat -s waylandbridge` 是宿主侧自我解释的地方。
