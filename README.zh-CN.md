# Astra HD + VR-CENTR

**Cyberpunk 2077 实验性 VR 移植：r30 / Center05 实验版本，2026 年10月19日。** Astra HD 将画面送入头显；中心安装一致的组件组合、保存独立配置档并启动选定内容。项目仍未完成。

> 普通R30的新建角色和镜子编辑器已在Test3与VR Night City的OpenXR模拟器检查中显示双眼、角色和UI。**但两个配置档都确认：退出编辑器后，显示器中的世界仍可见，双眼composed画面持续黑屏。这是R30已知且尚未修复的限制，原因尚未确定。** R30完整配置档流程、物理头显舒适性、稳定帧生成和目标帧率尚未通过验收。此前用户验收的是R29/Test3镜子编辑器和手动VR摄像机。
>
> 本发布提供下列两个配套runtime ZIP及校验和。[此前的r29/Center04](https://github.com/konstanaft-stack/Astra-HD/releases/tag/astra-hd-r29-experimental-20261018)不含R30新建角色修复，仍有该路径的头显黑屏问题。

| 同一实验版本的两个包 | 内容 |
|---|---|
| [Astra HD r30 native-base ZIP](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r30-experimental-20261019/Astra-HD-r30-native-base-20261019-experimental.zip) | VR 移植、编辑器资源、必需框架和 NVIDIA SR/RR/G310.9.16.0 |
| [VR-CENTR Center05 Windows x64 ZIP](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r30-experimental-20261019/VR-CENTR-public-win-x64-20261019-experimental05.zip) | 可运行的中心、runtime 和配置档脚本 |

[安装说明](INSTALL.md) · [验证范围与限制](STATUS.md) · [SHA256校验和](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r30-experimental-20261019/SHA256SUMS.txt) · [新发布页面](https://github.com/konstanaft-stack/Astra-HD/releases/tag/astra-hd-r30-experimental-20261019) · [Русский](README.md)

GitHub Source code ZIP 仅含源代码，不能替代两个 runtime 包；游戏、私人存档和模组合集不包含在内。

## 配置档与安装顺序

反复测试 VR 模组和图形库可能留下混杂文件。中心保存所选组合与独立状态，帮助返回已保存的基础，减少手动删目录、Steam 恢复和重装；它不会使任意合集自动兼容 VR。

1. 使用自己的 Cyberpunk 2077 2.31，先以干净状态启动到主菜单，然后退出。
2. 解压两个包：native ZIP 根目录含 bundle.json，中心 ZIP 含 VR-CENTR。需要 [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows)，中心自带 .NET runtime。运行 `Start-VR-CENTR.cmd`，按提示输入实际游戏与解压 bundle 文件夹；不使用不存在的 `-BundleDirectory` 参数。
3. 在中心保存干净基础，创建并保留 Flat，再创建独立配置档并应用 R30。Native Apply 不会自动建立干净快照。
4. 返回时切换到 captured Flat。空文件映射的 flat installer fallback 保持禁用。切换配置档会恢复该档保存的端口版本，不会自动升级所有配置档；游戏关闭后在需要的配置档中明确应用 R30。

**首次 Native Apply 在没有保存或有效继承的模式时，会一次性应用推荐图形预设，即使 UserSettings.json 已存在。** 图形可能改变；之后保存的 Custom/推荐选择会保留。已识别 R29→R30 更新继承保存的 Custom；任意外部配置档不保证被识别。直接 installer 与中心首次初始化是不同阶段，详见 INSTALL。

## R30 / Center05 的变化和边界

R30 普通 Test3 镜子编辑器的 fresh raw/UI 连续推进至少4分30秒；这是画面更新证据，不是 FPS 测量。配方将必需的 `xr_hud_to_second_eye=1` 应用于实际读取的 INI，包括 Custom 与启动前路径；其他图形设置、PPD、AER 与校准不作为该必需键改写。

Center05 保留 public04 配置档修复，并包含 shared SR/RR 默认选择和 addedUtc 排序的 PS 修复；没有 common 时 native-base 仍可用。一次NightCity编辑器SIM观察到约20 real FPS、FG 0；Test3与VR Night City退出编辑器后均确认双眼黑屏，不宣称完整profile PASS。一次 Test3→已修改 NightCity 切换测得153.8566秒（约2分34秒，n=1），反向VR Night City→Test3切换测得121.605秒（n=1），32个受保护文件及83个原始存档无差异。这些都是该电脑和已有配置档的单次测量，不是世界加载时间或其他电脑的保证。创建配置档耗时未测量。

R29/Test3 镜子编辑器的 HMD 验收仍有效。R30 物理头显、全部配置档/合集、新电脑与80 FPS目标尚未验收。面板轻微遮挡由用户明确推迟处理；AER 光晕/闪烁和其他 UI/masking 现象未宣称修复。

**此 native-base 不含 optional OptiScaler/NR/MFG。NR 与帧生成协同是后续阶段，不是已验收功能。** 所选 NVIDIA SR/RR/G310.9.16.0 保持不变；个人本机 common 不代表公共 ZIP 包含额外组件。

## 编辑器摄像机

按住左手 grip：左摇杆上下控制高度，左右控制距离；右摇杆左右改变围绕角色的角度；左摇杆按下 L3 重置手动偏移。摄像机偏移重置不等于头显系统 recenter。

## 开发方向与来源

目标是连贯的 VR 体验：空间菜单、舒适编辑器、清晰 HUD、控制器交互，以及稳定的神经渲染与帧生成。这些是方向，不是已完成功能。

Astra HD 基于 [dariulone 的 CyberpunkVR Port](https://github.com/dariulone/cyberpunk-vr-port) 和 [dabinn 的 Tofu Express X](https://github.com/dabinn/cyberpunk-vr-port)。保留原作者、历史和许可证；源代码在 [vr-center/](vr-center/)。各组件许可证、notices、NVIDIA 自有条款与 usvfs 条件保留；NVIDIA runtimes 不重新授权为 MIT。新native包149216876字节/360文件，Center05包113105318字节/511文件；已完成全文件SHA解压回读、native CheckOnly和Center05 CPU/bootstrap检查。这些不等于完整游戏流程或物理头显验收；精确SHA256见发布校验和与STATUS。
