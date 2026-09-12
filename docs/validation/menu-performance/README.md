# 菜单与状态显示更新验证

日期：2026-09-12；本机 Windows、RTX 3060 Ti，Release 构建。

- `build.log`：部署完成；CTest 的 `core_regressions` 通过，包含跟踪、PID、线程生命周期、图像/NMS、预览状态五组检查。预览测试覆盖类别 1 的 78% 检测在类别 0 模式下不选中、类别切换后选中、IoU 失配、干运行/按键/测量过期/死区提示，以及暂停、无新帧、采集不可用时清除红圈。
- `menu-tests.log`：Windows PowerShell 5.1 执行 68 项检查通过；覆盖菜单路由、参数引用、设置、进程记录和脚本解析。此脚本自身不启动主应用。
- `runtime-smoke.json`：部署后的运行测试全部通过，包含无效输入、离线禁止注入、离线干运行、GDI/DXGI 采集、停止文件退出和引擎构建取消。
- `model-comparison.json`：6 张仓库样例的 ONNX Runtime CPU / TensorRT FP16 数值对照通过。这验证实现一致性，不是目标场景准确率评估。
- 实际在 PowerShell 菜单输入 `7 → 4`，GPU、GDI、DXGI 三项按顺序自动完成，结果保存在 `menu-sequential-quiet-desktop.json`；之后输入 `0` 正常退出。所有模式 `input_enabled=false`，实时两项 `sent_count=0`。
- 用仓库 `gril.jpg` 实测两种类别：类别 0 时 1 个检测框、0 个候选、无有效目标；类别 1 时 1 个检测框、1 个候选、有有效目标。已查看两张最终预览，状态说明、类别百分比与红圈行为符合预期。输出在 `.local/status-demo/sample-body` 和 `sample-head`。
- 含空格及中文的本机图片路径 `.local/status-demo/中文 路径/识别 样例.jpg` 实际解码与推理通过。不据此宣称任意 Unicode 字符、任意视频格式或其他系统代码页都已支持。

`gpu.json`、`gdi-headless.json`、`dxgi-headless.json`、`gdi-preview.json` 是此前同次开发过程的分模式原始测量。完整解释见 [性能说明](D:/Projects/Test_Study_Yolo/docs/performance-explained.md)。较静止桌面的菜单整组测试与前一组桌面更新状态不同，因此 DXGI FPS 显著不同；不能混为重复条件下的结果。

本次验证没有开启真实鼠标控制模式。此前完成的本机鼠标标定与十二项改动记录见 [部署报告](D:/Projects/Test_Study_Yolo/docs/implementation-report.md)。
