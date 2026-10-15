# Astra HD

[![Русский](https://img.shields.io/badge/Русский-2563eb?style=for-the-badge)](README.ru.md)
[![English](https://img.shields.io/badge/English-2563eb?style=for-the-badge)](README.md)
[![中文](https://img.shields.io/badge/中文-2563eb?style=for-the-badge)](README.zh-CN.md)

**《赛博朋克 2077》VR · 持续开发中**

Astra HD 是 [dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port) 的实验性分支。我们希望实现舒适的完整 VR 游戏体验：一致的双眼立体画面、清晰的图像，以及适合在头显中使用的界面。

> **Astra HD 尚无稳定发行版。** 当前公开仓库包含上游代码基础和项目说明。Astra HD 的专属改动仍在本地开发与验证中；继承自上游的文件并不代表经过验证的 Astra HD 安装包。

## 开发与验证

| 项目 | 当前状态 |
| --- | --- |
| 游戏世界的双眼输出 | 已在本地 OpenXR 模拟器会话中验证；更全面的实体头显验收仍未完成 |
| 物品栏中的角色立体显示 | 已在本地模拟器中进行图像验证 |
| 外观编辑器的立体显示 | 10 月 15 日已在 OpenXR 模拟器中验证双眼输出；仍待实体头显验收 |
| NVIDIA 帧生成集成 | 已在部分本地场景中验证；性能、界面切换和兼容性仍在测试 |
| 设置持久化 | 启动时设置重置的问题尚未解决 |
| 稳定通关整部游戏 | 尚未验证 |

当前重点是角色编辑器的立体显示、可靠的图像输出、帧生成及设置保存。后续将改进 HUD 布局、导航、武器选择和其他 VR 交互。

模拟器结果用于支持开发。舒适度、运动表现、图像质量和性能还必须在实体头显中检验。经过验证的版本发布时，我们会提供 Astra HD 安装说明和二进制文件。

## 项目来源与致谢

- [dariulone/cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port)：原始项目，也是本分支的直接上游。
- [dabinn/cyberpunk-vr-port — Tofu Express](https://github.com/dabinn/cyberpunk-vr-port)：我们本地开发的重要基础。
- 后续采用其他社区贡献者的代码时，将逐项注明来源。

保留原始提交历史和作者声明。已有组件继续遵循各自的许可证。

更新于 2026 年 10 月 15 日。

[2026 年 10 月 15 日：编辑器立体显示进展、图像和修复补丁](STEREO-EDITOR-2026-10-15.md)
