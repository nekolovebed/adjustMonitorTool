# 免责声明 / Disclaimer

> 最后更新：2026-11-25
> 适用于本仓库（`nekolovebed/adjustMonitorTool`）及其发布的 `Luma.exe` 等全部产物。

## 1. 接受条款

下载、安装、运行或以任何方式使用本软件，即视为你已阅读、理解并同意本声明全部内容。**如果你不同意，请立即停止使用并删除本软件。**

## 2. 按「现状」提供，无任何担保

本软件按**「现状」（AS IS）**提供，不附带任何明示或暗示的担保，包括但不限于对**适销性、特定用途适用性、无瑕疵、无病毒、不侵权**的担保。作者不保证本软件能正常运行、不中断、无错误，也不保证它能识别或控制你的任何一台显示器。

## 3. 使用风险由你承担

本软件通过显示器厂商公开支持的 **DDC/CI**（VCP 指令 `0x10`）与 **WMI** 接口，直接读写显示器的背光寄存器。这属于厂商设计内的正常操作，但：

- 不同显示器、显卡、转接设备的 DDC 实现差异很大，个别设备可能出现**黑屏、闪烁、亮度跳变、色彩异常、设置不生效或无法恢复**等情况；
- 显示器固件缺陷、驱动异常、热插拔时机不当，也可能导致上述现象；
- 笔记本内屏通过 WMI 调节，若系统电源策略或厂商工具同时接管，可能出现亮度来回跳动。

**请在充分了解风险后使用，并在开始调节前确认你能用显示器实体按键（OSD）恢复亮度。** 由此产生的一切后果由使用者自行承担。

## 4. 责任限制

在适用法律允许的最大范围内，作者**不对**因使用或无法使用本软件而造成的任何直接、间接、附带、特殊、惩罚性或后果性损失承担责任，包括但不限于：显示器或其他硬件损坏、设备不可用、工作中断、数据丢失、利润损失、商誉损失。

若部分司法辖区不允许排除或限制某些责任，则上述限制在该辖区法律允许的最小范围内适用。

## 5. 合规使用

使用者应自行确保对设备拥有合法使用权，并遵守所在地法律法规及设备厂商的使用条款。**不得**将本软件用于任何违法用途，或用于你无权操作的设备。

## 6. 隐私与数据

- 本软件**不联网**，不发起任何网络请求，不上传、不收集、不统计任何数据。
- **不写入注册表**，不创建计划任务与服务，不在系统目录留下文件。
- 仅在运行时于内存中读取显示器信息，退出即释放。
- 唯一可选的持久化行为是：用户自行把快捷方式放入「启动」文件夹以实现开机自启。

## 7. 第三方、商标与来源

- 本项目为**独立实现**，未使用、未复制任何第三方开源项目的代码，不包含任何 GPL/LGPL 代码。
- 与任何第三方软件**无任何关联、无隶属关系**，不包含其任何代码、图标、字体或其它资源。
- 「Windows」是 Microsoft Corporation 的商标；文中出现的其他软件、硬件、品牌名称归各自所有者所有。
- 本项目与 Microsoft 及任何显示器厂商**无隶属关系，也未获其背书**。

## 8. 许可

本软件以 **MIT License** 发布，详见 [LICENSE](LICENSE)。你可以自由使用、修改、再分发，只需在副本或实质性部分中**保留版权声明与本许可声明**（即注明来源）。

## 9. 声明变更

作者可能随时更新本声明，更新后随仓库一同发布，不另行单独通知。继续使用即视为接受更新后的声明。

---

## English Summary

This software is provided **"AS IS"**, without warranty of any kind. It writes monitor backlight values through vendor-supported **DDC/CI** (VCP `0x10`) and **WMI** interfaces. Monitor implementations vary widely; black screens, flickering, or settings that cannot be restored are possible. **Use at your own risk** and make sure you can restore brightness with the monitor's physical OSD buttons. The author is not liable for any direct or indirect damages, including hardware damage or data loss, arising from the use of this software.

The software makes **no network requests**, collects **no data**, and writes **no registry keys**.

This project is an independent implementation and is **not affiliated with** any third-party software, Microsoft, or any display manufacturer. "Windows" is a trademark of Microsoft Corporation. Licensed under the **MIT License**; redistribution only requires keeping the copyright and license notice.
