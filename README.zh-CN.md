# Astra HD + VR-CENTR

> **r29 的阻断性问题：新游戏（2026-10-19）。** 创建新角色时，显示器有画面，但头显中为黑屏；已在 Test3 和 VR Night City 中确认。已发布的 ZIP 尚未包含修复，目前不适合从新游戏开始。此前的验收仅限通过镜子进入的编辑器。

**Cyberpunk 2077 的实验性 VR 移植；当前安装包为 r29 / Center04（2026 年 10 月 18 日）。** Astra HD 将游戏画面送入头显，VR-CENTR 负责安装、启动和独立配置档。项目还远未完成，功能与操作可能继续改变。

| 下载同一版本的两个安装包 | 内容 |
|---|---|
| [Astra HD r29 native-base ZIP](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r29-experimental-20261018/Astra-HD-r29-native-base-20261018-experimental.zip) | VR 移植、编辑器资源、必需框架和 NVIDIA SR/RR/G 310.9.16.0 |
| [VR-CENTR Center04 Windows x64 ZIP](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r29-experimental-20261018/VR-CENTR-public-win-x64-20261018-experimental04.zip) | 可运行的管理器、runtime 和配置档管理脚本 |

[安装说明](INSTALL.md) · [实际验证范围与限制](STATUS.md) · [校验和](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r29-experimental-20261018/SHA256SUMS.txt) · [发布页面](https://github.com/konstanaft-stack/Astra-HD/releases/tag/astra-hd-r29-experimental-20261018) · [Русский](README.md)

GitHub 自动生成的 Source code ZIP 只包含源代码，不能替代上面的两个安装包。游戏本体、个人存档和模组合集不包含在内。

## 为什么配合配置档使用

反复测试 VR 模组和图形库可能留下混杂的文件。管理器保存所选组合与独立状态，帮助恢复已保存的基础，减少手动删除目录、通过 Steam 重新下载和反复重装。新的干净配置档需要预先保存首次启动到主菜单后的干净游戏。

## 新电脑上的基本顺序

1. 安装自己的 Cyberpunk 2077，先以干净状态启动到主菜单，然后退出。
2. 下载并解压两个安装包；native ZIP 根目录包含 bundle.json，管理器 ZIP 包含 VR-CENTR 文件夹。需要 PowerShell 7，管理器自带 .NET runtime。
3. 在管理器保存干净基础，创建并保留 Flat 配置档，再创建单独的配置档并应用 r29。
4. 返回干净状态时切换到已保存的 Flat 配置档。Native Apply 不会自动替你建立干净备份；请按 [INSTALL](INSTALL.md) 操作。

**首次 Native Apply 在该版本尚无已保存模式时，会一次性应用推荐图形预设，即使 UserSettings.json 已存在。** 图形选项可能改变；之后已保存的 Custom / 推荐模式选择会保留。直接 installer 对已有 settings 文件的处理与管理器首次初始化不同，详见 INSTALL。

## r29 的验证范围

新的双摄像机编辑器已检查两眼图像、独立 UI、皮肤→嘴唇→预览→皮肤切换、手动视角与头部参考的保持，以及重置。**2026 年 10 月 19 日，用户在 Test3 配置档中使用物理头显验收了 r29 编辑器的画面与手动 VR 摄像机控制，确认工作正常。** 面板对视野的轻微遮挡由用户明确推迟处理，不阻碍此次编辑器验收。

此次验收仅限 Test3 编辑器，不代表已测得 FPS、所有配置档或模组合集均已通过，或 NR+FG 和完整 VR 体验已经完成。交替 AER 模式的光晕与闪烁未宣称修复。

**此 native-base 不含 optional OptiScaler/NR/MFG。NR 与帧生成协同工作是后续阶段，不能视为本次发布已完成功能。** 历史本机安装包含的额外组件不代表这些组件也在公共 ZIP 内。

## 编辑器摄像机

按住左手 grip：左摇杆上下控制高度，左右控制距离；右摇杆左右改变围绕角色的角度；左摇杆按下 L3 重置手动偏移。摄像机偏移重置不等于头显系统 recenter。

## 开发方向与来源

目标是连贯的 VR 体验：空间菜单、舒适编辑器、清晰 HUD、控制器交互，以及稳定的神经渲染与帧生成。这些是开发方向，不是已完成的功能清单。启动及配置档切换时间的现有测量和限制见 STATUS。

Astra HD 基于 [dariulone 的 CyberpunkVR Port](https://github.com/dariulone/cyberpunk-vr-port) 和 [dabinn 的 Tofu Express X](https://github.com/dabinn/cyberpunk-vr-port)。保留原始作者、历史和许可证；管理器源代码位于 [vr-center/](vr-center/)。各组件的许可证和 notices 随安装包提供，NVIDIA runtimes 不重新授权为 MIT。

