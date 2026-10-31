# VR：在一个 anland 窗口里跑 SteamVR

anland 把每个 Wayland toplevel 变成一个 Android 窗口。在容器看来，SteamVR 的
`vrcompositor` 只是“又一个带窗口的客户端”——所以这里的 VR 路径不是把显示交出去：
合成器继续往自己的 toplevel 里画，守护进程在把窗口贴到手机或 AR 眼镜上的时候，
顺手把这张图掰成左右两只眼。

这样做可以复用 anland 已有的一切（dma-buf 导入、EGLImage、attach/detach 生命周期），
并且始终让 SurfaceFlinger 负责显示。代价是：anland 之外的东西没法驱动头显——
姿态必须从 Android 回来，而且必须让 SteamVR 走“窗口化呈现”，而不是抢占 DRM 显示。

```
Android                          host daemon                container
┌──────────────┐  binder  ┌───────────────────────┐  wayland  ┌───────────────┐
│ AwlXrSession │ ───────► │ awl_xr.cpp            │ ◄──────── │ vrcompositor  │
│ IMU + 手柄   │  姿态    │  • 镜头畸变           │  dmabuf   │ (SBS 或单眼)  │
│ (或 6DoF     │  输入    │  • 重投影(timewarp)   │           │               │
│  提供者)     │  姿态    │  • anland-xr-display  │           └───────────────┘
└──────────────┘          │  • anland-xr-pose     │ ────────►  driver_anland.so
                          └───────────────────────┘  seqlock   (HMD + 2 手柄)
```

## 新增了什么

| 部件 | 位置 | 作用 |
| --- | --- | --- |
| `AwlXrSession` | `app/libawl` | 从 IMU 取 3DoF 头姿（同时算角速度）、手柄轴与按键，经 binder 以约 250 Hz / 60 Hz 上报 |
| `awl_xr.cpp` | `services/waylandbridge` | 标记 XR 窗口、执行畸变 + 重投影、发布 `anland-xr-*` 文件、接管可选的 6DoF 提供者 |
| `anland_xr_wire.h` | `xr/include` | 共用文件格式（显示配置、姿态块、输入块） |
| `driver_anland.so` | `xr/driver` | OpenVR 服务端驱动：头显（`IVRDisplayComponent`）+ 两个手柄 |
| `libVkLayer_anland_vr.so` | `xr/layer` | 隐式 Vulkan 层，XR 会话期间隐藏 `VK_KHR_display` |
| `anland-steamvr` | `xr/` | install / status / start / stop 辅助脚本 |
| `xr_wire_test` | `xr/tools` | 显示/姿态/输入文件格式的自测（`xr/build.sh --test`，`xr/build.sh` 与 CI 也会跑） |

## 前提条件

- anland 6.x，使用本仓库的守护进程（XR 钩子在 `waylandbridge` 里）与本仓库的宿主
  APK（XR 会话在 `libawl` 里）。
- `sc_enabled = 0`。XR 的绘制在 EGL 渲染器里做；SC 后端没有这个钩子，只会把原始
  窗口直接显示出来。
- 容器里有 SteamVR 2.x。`xr/build.sh` 是按“容器自己的架构”编译的，所以要在
  容器里编（手机上是 aarch64，桌面测试机上是 x86_64）——NDK 编不了这些，
  容器是 glibc。
- 容器里有一个能用的 Vulkan ICD（Turnip/KGSL）。这个层存在的唯一原因，就是
  Turnip 处理不好的 `VK_KHR_display`。

## 打开它

这些也都能在手机上改：**窗口设置 → VR**。设置页通过 binder
（`CFG_GET`/`CFG_SET`，标题匹配表另走字符串事务）读取守护进程里的真值，
写回 `config.json` 并立即生效——XR 窗口会收到新的 `C_XR` 并重绘，所以可以一边跑
SteamVR、一边透过镜片调畸变。Quest 3S 预设按钮会把下面那组几何参数填进去。

配置文件依然可用，适合脚本和第一次开机：
`/data/adb/modules/anland-awl/config.json`（改完 `stop && start` 模块服务，或重启）：

```json
{
  "sc_enabled": 0,
  "xr_enabled": 1,
  "xr_mode": 2,
  "xr_fov": 90,
  "xr_ipd": 63,
  "xr_k1": 0,
  "xr_k2": 0,
  "xr_predict": 20
}
```

所有 XR 值都是整数：百分比和镜头系数用千分比，所以 `xr_k1 = 150` 表示
shader 里的 `k1 = 0.15`。

键表（除 `xr_enabled` 外都可选）：

| 键 | 默认 | 范围 | 含义 |
| --- | --- | --- | --- |
| `xr_enabled` | `0` | 0–1 | 打开 XR 路径 |
| `xr_mode` | `2` | 0–3 | `1` 单眼（一张图铺满屏）、`2` 左右水平、`3` 上下垂直、`0` 关闭这一趟绘制 |
| `xr_refresh` | `0` | 0–240 | 覆盖头显刷新率（Hz）；`0` = 跟面板 |
| `xr_eye_w`、`xr_eye_h` | `0` | 0–4096 | 每眼渲染尺寸；`0` = 跟随 XR 窗口的 surface |
| `xr_fov` | `90` | 30–170 | 每眼水平 FOV（度），垂直方向按眼睛宽高比推 |
| `xr_ipd` | `63` | 20–90 | 瞳距（毫米） |
| `xr_scale` | `1000` | 200–2000 | 千分比的每眼尺寸缩放 |
| `xr_k1`、`xr_k2` | `0` | −1000–2000 | 千分比的桶形畸变系数；`0` = 不做镜头校正，直接透传 |
| `xr_ca` | `0` | 0–100 | 千分比的色散补偿 |
| `xr_predict` | `0` | 0–120 | 在最新姿态之外外推多少毫秒的头部运动；`0` = 不预测 |
| `xr_match` | `vrcompositor,steamvr` | — | 逗号分隔、不区分大小写的子串，命中即视为 XR 窗口 |

`xr_k1`/`xr_k2` 是给 cardboard 那种透镜用的；AR 眼镜、或者直接看手机屏幕时，
把它们留在 `0`，这一趟绘制就退化成一次普通（但仍然经过重投影的）贴图。

### Meta Quest 3S

Quest 3S 是双面板 Android 设备：每眼 1832×1920，默认 90 Hz（可开 120 Hz），
每眼大约 96°×90°，标称瞳距 63 mm。要在头显本机上跑 anland，这样 XR 窗口
*就是*显示器，而面板要透过菲涅尔透镜看——也就是说桶形系数必须非零，
不像眼镜那种情况。

```json
{
  "sc_enabled": 0,
  "xr_enabled": 1,
  "xr_mode": 2,
  "xr_refresh": 90,
  "xr_eye_w": 0,
  "xr_eye_h": 0,
  "xr_scale": 900,
  "xr_fov": 96,
  "xr_ipd": 63,
  "xr_k1": 220,
  "xr_k2": 40,
  "xr_ca": 10,
  "xr_predict": 20
}
```

- `xr_eye_w/h = 0` 表示跟随窗口：守护进程采用宿主给 XR 窗口的 surface 尺寸并重新
  发布显示配置，所以每眼目标不会和合成器的 swapchain 脱节。如果算出来的宽高比与
  `xr_mode` 矛盾，守护进程会打一条警告，而不是默默拉伸。
- `xr_scale = 900` 表示略低于原生分辨率渲染，剩下的交给重投影去拉伸——这是
  `vrcompositor` 报掉帧时第一个该调的旋钮。
- 上面的 `k1`/`k2` 是 Q3S 透镜的**起点**，不是实测值：找一张直线网格
  （SteamVR 的房间边界，或 `steamvr.vrsettings` 里 `driver_anland` 的测试图），
  把 `k1` 一点点加大，直到边缘不再向外鼓。`xr_ca` 用来压掉边缘的红蓝镶边。
- `xr_predict = 20` 表示用 IMU 的角速度把姿态外推 20 ms。转头时调它：
  太小世界会“跟得慢”，太大就会过冲。0 表示关闭预测。
- Horizon OS 只通过 OpenXR 把原始 IMU 交给 VR 应用，不会给普通 2D Activity。
  如果 `./anland-steamvr status` 显示 `seq` 一直在涨、而四元数始终是
  `0,0,0,1`，说明头显不给传感器：可以把 `xr_mode` 设成 `1` 用 SteamVR 的
  座椅模式，或让 OpenXR 桥通过 `anland-xr-pose-ext`（见下）喂姿态，或者把设备
  root 到能把 `SensorManager` 暴露给宿主 APK 的程度。其它部分（SBS 输出、
  畸变、手柄）在任何一种情况下都能用。
- 无论如何 root 都是硬前提：`anland` 是一个 KernelSU/SukiSU 模块，加上一个跑在
  `awl_daemon` 域的 binder 守护进程。

然后在容器里：

```sh
tar xzf anlandx.tar.gz && bash anlandx/setupanlandx.sh   # 照旧，只做一次
cd anlandx/xr
./build.sh                    # 先跑格式自测，再编 driver + layer 到 xr/build/
./anland-steamvr install      # -> SteamVR/drivers/anland、~/.local/share/vulkan
./anland-steamvr start        # 带 ANLAND_VR_LAYER_ENABLE=1 启动 vrstartup.sh
```

`install --system` 会把层装到 `/usr/share/vulkan/implicit_layer.d`，而不是用户目录。
`anland-steamvr status` 会打印守护进程当前发布的内容，头显黑屏时第一件事就是看它。

## 一帧是怎么被掰弯的

1. `vrcompositor` 创建自己的 toplevel；`cb_window_title()` 发现标题命中
   `xr_match`，把这个窗口标记为 XR。
2. 守护进程通过该窗口的控制 binder 发 `AWL_C_XR`（模式、刷新率、每眼尺寸、FOV、
   瞳距）。`AwlWindowActivity` 启动 `AwlXrSession`。
3. 每个 IMU 采样都通过 `AWL_T_XR_POSE2` 回来（四元数、位置、角速度），手柄状态走
   `AWL_T_XR_INPUT`。守护进程把最新姿态放进 seqlock，并镜像到
   `$ANLAND_RUNTIME_DIR/anland-xr-pose`，输入镜像到 `anland-xr-input`。
4. 驱动的 `RunFrame()` 读这两个文件喂给 OpenVR。它的 `IVRDisplayComponent` 报告
   `IsDisplayOnDesktop() = true`、`IsDisplayRealDisplay() = false`，并且返回
   恒等畸变：弯曲由宿主做，合成器不能做第二遍。
5. `vrcompositor` 交换 buffer 时，守护进程的 GL 渲染器取 XR 窗口的第一个图层，
   用 `awl_xr_draw()` 取代原来的 quad：先做每眼桶形预畸变，再在切线空间里施加一个
   旋转（`q_now ⊗ conj(q_at_frame)`）——这就是让 60 fps 的画面能跟住 250 Hz 姿态的
   重投影。`q_now` 是最新姿态（可选地按 `xr_predict` 外推过），`q_at_frame` 是
   这个窗口上一帧绘制时用的姿态，所以多个 XR 窗口各按自己的参考帧重投影。
6. 手柄：驱动加两个 `vive_controller` 档位的设备，位姿 = 头姿 + 固定的肩部偏移；
   如果输入块里带了它们的真实位姿，就直接用真实位姿（见下）。

## 外部 6DoF 提供者

IMU 会话只有 3DoF，因为手机没有 SLAM。而**有**追踪能力的东西——手机上的 ARCore
Activity、头显上的 OpenXR 桥、独立的 6DoF 追踪守护进程——不用改 anland 的代码，
只要往 `$ANLAND_RUNTIME_DIR/anland-xr-pose-ext` 里写，就能接管姿态：

```c
struct anland_xr_pose_block* blk = /* mmap ANLAND_XR_POSE_EXT_FILE 的 4096 字节 */;

anland_xr_seq_begin(&blk->seq);
blk->magic          = ANLAND_XR_POSE_MAGIC;
blk->pose.sample_ns = now_ns;          /* CLOCK_MONOTONIC，严格递增 */
blk->pose.quat[4]   = q;               /* x, y, z, w —— +x 右、+y 上、-z 前 */
blk->pose.pos[3]    = p;               /* 米；房间尺度的位置 */
blk->pose.ang_vel[3]= w;               /* rad/s，可选（0 = 由守护进程差分） */
blk->pose.tracking  = 1;               /* 0 = 提供者当前没有定位 */
anland_xr_seq_end(&blk->seq);
```

守护进程按这些规则处理：

- 只有 `sample_ns` 比已有姿态更新时才生效，所以同一个采样重复写是无害的。
- 只要这个文件还在更新（250 ms 内），`AwlXrSession` 的 IMU 姿态就会被丢弃——
  两个不同频率的源混在一起会互相打架，重投影就会抖。
- `tracking = 0`、文件消失、或静默超过 250 ms，姿态就交还给 IMU 会话。不需要
  拆任何东西，下一个 IMU 采样自然又生效。
- 文件由守护进程以 `O_RDONLY` 打开、并且**永远不会创建**——它的生命周期归提供者。
  想永久回到 3DoF，删掉它就行。

同样的办法在低一层也能用：如果有东西往 `anland-xr-input` 里写真实的手柄位姿
（`c[i].quat`/`c[i].pos` 不是单位值），驱动就会用真实位姿，而不是“头姿 + 肩部偏移”。

## 验证一次会话

- `anland-steamvr status` —— 打印出一段显示配置，再加
  `pose: seq=… quat=(…)`，说明整条链是活的。`display: <missing>` 说明守护进程
  没带 `xr_enabled=1` 在跑（或者改完之后没重启）。
- `/data/local/tmp/awl_daemon.log` —— 每个 XR 窗口一行 `xr: window <id> …`；
  收到 surface 尺寸时有 `xr: render target follows the window: …`；
  `anland-xr-pose-ext` 接管时有 `xr: external pose provider is live …`。
- 宿主上 `logcat -s AwlWindowActivity` —— `applyXrState` 会打印它拿到的几何参数。
- `cd anlandx/xr && ./build.sh --test` —— 在任何 Linux 机器上检查显示/姿态/输入
  的文件格式，不需要手机。
- 如果 SteamVR 起来了、但 HMD 页显示 “no headset”，检查驱动目录名：SteamVR 找的是
  `drivers/<name>/bin/<platform><arch>`，arm64 Linux 用的平台名是 `linuxarm64`。

## 已知限制

原来那七条，现在各自到了什么程度：

| 限制 | 现状 |
| --- | --- |
| **只有 3DoF** | **已收窄。** 任何 6DoF 源都可以写 `anland-xr-pose-ext`，守护进程优先用它的姿态（含位置）；没有这样的源时，IMU 会话仍然只有朝向。anland 自己不实现 SLAM。 |
| **姿态延迟高** | **已收窄。** `xr_predict`（0–120 ms）用角速度外推，驱动里的 `vecAngularVelocity` 和重投影都吃这个。它掩盖的是姿态**传输**延迟；Android 传感器管线和多一层宿主合成的开销仍在。 |
| **只能有一个 XR 窗口** | **已收窄。** 重投影参考帧改成按窗口保存，第二个 XR 窗口按自己的上一帧重投影，不会重复旋转。所有 XR 窗口仍共享同一个头姿，实际能当头显视图的也只有一个。 |
| **窗口就是渲染目标** | **已收窄。** `xr_eye_w/h = 0` 时每眼目标跟随 XR 窗口的 surface 尺寸，每次改变都重发；宽高比不匹配时打一条警告，而不是默默拉伸。swapchain 尺寸仍由 SteamVR 决定。 |
| **手柄只是伪装成手柄** | **部分解决。** 文件格式和驱动都接受真实的手柄位姿；但原生 Android 没有能产生这些位姿的追踪源。按键/摇杆在两种情况下都映射到 `vive_controller` 档位。 |
| **没有 direct mode** | **设计上就不做，这里解决不了。** direct mode 需要 `IVRDriverDirectModeComponent`、拦截 Vulkan present、以及虚拟显示，恰恰是 anland 拒绝的那套：SurfaceFlinger 必须继续当合成者。硬性依赖它的老标题找不到它。 |
| **原型状态** | **已收窄。** `xr/tools/xr_wire_test.c` 把显示文件和两个 seqlock 块都做了往返测试，`xr/build.sh` 和 CI 都会跑。设备侧表现（畸变数值、与 SteamVR 的配合）仍未在真机验证。 |

XR 路径仍然**故意**不做的事：

- **接管显示。** direct mode、硬件直出、每眼 `VK_KHR_display` 平面，都是 anland
  模型的反面。
- **追踪手柄。** 没有摄像头、没有灯塔输入；追踪桥可以把位姿喂进来，但 anland
  本身不产生位姿。
- **真正预测未来。** `xr_predict` 只是在最新采样上做匀速外推，不是带加速度的
  滤波器，频率取决于 IMU 或提供者跑多快。
