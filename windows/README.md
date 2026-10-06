# Compositor Windows 版

这是 [Compositor](../README.md) 的 Windows 版本：一个面向合成与修图、类似 Photoshop 操作习惯的免费开源图像编辑器。

原版是 macOS 专属应用（SwiftUI、AppKit、Metal、Core Image），没法直接移植，所以 Windows 版用 **C++17 与 Qt 6** 重写了界面和渲染，同时**直接复用**原版 `Compositor/Rendering/` 下的 C 像素内核（调整、颗粒、杂色、黑白、色彩平衡、渐变映射等），保证两边算出来的像素一致。

项目文件 `.comp` 两边通用：在 Windows 上保存的项目可以在 Mac 上打开，反过来也一样。

## 下载

每次推送都会由 GitHub Actions 自动构建（工作流 **Windows**），在运行记录的 Artifacts 里下载：

- `Compositor-Windows-x64`：便携版 zip，解压后直接运行 `Compositor.exe`，已附带 Qt 与 VC++ 运行库。
- `Compositor-Windows-x64-Setup`：安装包，带开始菜单快捷方式和卸载程序。

系统要求：64 位 Windows 10 或 11。

## 已经可以用的功能

**项目与文件**
- 打开、编辑、保存 `.comp` 项目（格式版本 1–11，与 macOS 版相同的校验规则）
- 多个项目分标签页同时打开
- 打开 PNG、JPEG、BMP、GIF、TGA、PSD（合成图）、WebP、TIFF 作为新画布，或置入为图层
- 导出 PNG（带分辨率信息）和 JPEG（可选品质与透明处填充色）
- 剪贴板：拷贝图层、合并拷贝、粘贴图片为新图层；把图片拖进窗口置入
- 项目被其他程序（例如 AI 代理）改写时自动重新载入；有未保存修改时先询问
- 保存时就地原子更新：先写图片、最后替换清单，没变的图片不重写
- 保存时生成 `QuickLook/Preview.jpg`，项目拷到 Mac 上访达也能预览

**图层**
- 像素图层、文件夹（含文件夹不透明度与文件夹蒙版）、调整图层
- 24 种混合模式（Photoshop 顺序与公式）、不透明度
- 图层蒙版：添加、停用、反相、应用、删除，用画笔在蒙版上绘制
- 剪贴蒙版（Ctrl+Alt+G）
- 新建、复制、删除、编组、向下合并、拼合图像、拖放排序、重命名、显示隐藏
- 图层效果：描边（内/外）、投影、颜色叠加、内阴影、外发光、内发光，可随时修改、隐藏，沿蒙版后显示的形状计算
- 文字图层、形状等 macOS 版的元数据会原样保留；文字以保存的像素显示

**调整图层**（全部可编辑、实时预览）
- 色相/饱和度（含六个色彩范围与着色）、色阶、曲线、曝光度、渐变映射、颗粒、添加杂色、高斯模糊、动感模糊、反相、黑白、色彩平衡

**选区**
- 矩形与椭圆选框（M）、自由与多边形套索（L）、魔棒（W，容差、连续、对所有图层取样）
- 按下时 Shift 添加、Alt 减去、Shift+Alt 交叉；全选、取消选择、反选、羽化、扩展、收缩、载入图层选区
- 有选区时：清除（Delete）、填充、剪切与拷贝只取选区，内容识别填充，裁剪到选区，添加蒙版与新建调整层以选区为蒙版，画笔类工具只在选区内落墨

**工具**
- 移动（V）：拖动移动；拖动手柄缩放（Shift 等比，Alt 以中心）；在角外拖动旋转（Shift 每 15°）；方向键微移；拖动参考线
- 画笔（B）与橡皮擦（E）：大小、硬度、不透明度，Shift 点击画直线，同一笔不会越描越深
- 污点修复画笔（J）：内容识别、创建纹理、近似匹配
- 仿制图章（S）：Alt 点击取样，可对齐，可对所有图层取样
- 渐变（G）：线性或径向，前景到背景或到透明，可反向，可画在蒙版上
- 吸管（I）：Alt 点击取背景色
- 抓手（H，或任何工具下按住空格）、缩放（Z）

**图像**
- 画布大小（九宫格定位）、图像大小（非破坏，保留原始像素）、水平/垂直翻转画布与图层

**视图**
- 缩放、适合屏幕、100%（按物理像素，支持高分屏），缩小时用多级缩小图保证清晰
- 标尺（Ctrl+R），从标尺拖出参考线，拖回标尺删除；新建与清除参考线
- 放大到 800% 以上显示像素网格

## 还没有的功能

下面这些 macOS 版有、Windows 版暂时没有，打开含有它们的项目不会丢数据，只是不能编辑或不显示：

- 文字工具（文字图层能显示和保存，不能改字）
- 选择主体、移除背景（macOS 版用的是系统的 Vision 框架）
- 移动选区里的像素、自由扭曲
- 模糊工具、形状工具、涂抹与液化
- Camera Raw 滤镜和“滤镜”菜单里的破坏性滤镜
- 分层导入 PSD、导入相机 RAW 与 HEIC
- 布局网格、吸附到参考线与图层边缘
- 自定义快捷键、自动更新

## 快捷键

| 操作 | 快捷键 |
|---|---|
| 新建 / 打开 / 保存 / 另存为 | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| 打开项目文件夹 | Ctrl+Shift+O |
| 置入图片 | Ctrl+Shift+P |
| 导出 PNG / JPEG | Ctrl+Alt+E / Ctrl+Shift+Alt+S |
| 撤销 / 重做 | Ctrl+Z / Ctrl+Shift+Z 或 Ctrl+Y |
| 拷贝图层 / 合并拷贝 / 粘贴 | Ctrl+C / Ctrl+Shift+C / Ctrl+V |
| 新建图层 / 复制图层 / 编组 | Ctrl+Shift+N / Ctrl+J / Ctrl+G |
| 向下合并 / 剪贴蒙版 | Ctrl+E / Ctrl+Alt+G |
| 上移 / 下移图层 | Ctrl+] / Ctrl+[ |
| 用前景色填充 | Alt+Backspace |
| 画布大小 / 图像大小 | Ctrl+Alt+C / Ctrl+Alt+I |
| 放大 / 缩小 / 适合屏幕 / 100% | Ctrl+= / Ctrl+- / Ctrl+0 / Ctrl+1 |
| 画笔变小 / 变大 | [ / ] |
| 交换 / 复位前景背景色 | X / D |
| 全选 / 取消选择 / 反选 | Ctrl+A / Ctrl+D / Ctrl+Shift+I |
| 羽化 | Shift+F6 |
| 清除选区内容 / 剪切 | Delete / Ctrl+X |
| 内容识别填充 | Shift+Backspace |
| 图层效果 | Ctrl+Shift+F |
| 显示标尺 | Ctrl+R |
| 工具 | V 移动，M 选框，L 套索，W 魔棒，B 画笔，E 橡皮，J 修复，S 仿制，G 渐变，I 吸管，H 抓手，Z 缩放 |

## 关于 .comp 项目文件夹

`.comp` 在 Mac 上是“文件包”，在 Windows 上就是一个普通文件夹，里面有 `manifest.json` 和 `images/`。打开方式：

- 菜单“打开项目文件夹…”（Ctrl+Shift+O）选中 `.comp` 文件夹；
- 或者“打开…”后进入文件夹选 `manifest.json`；
- 或者直接把 `.comp` 文件夹拖到窗口里。

AI 代理和脚本可以直接读写项目，规则见 [docs/writing-comp-files.md](../docs/writing-comp-files.md)；格式细节见 [docs/project-format.md](../docs/project-format.md)。

## 命令行工具

安装目录里的 `compositor-cli.exe` 不打开界面就能处理项目：

```
compositor-cli info 项目.comp                      显示画布与图层结构
compositor-cli validate 项目.comp                  校验项目能否被打开
compositor-cli render 项目.comp 输出.png [--scale 0.5]
compositor-cli render 项目.comp 输出.jpg [--quality 90]
compositor-cli new 新项目.comp 1920 1080 [--fill 336699]
compositor-cli import 照片.jpg 新项目.comp
```

## 从源码构建

需要 Visual Studio 2022（含“使用 C++ 的桌面开发”）、CMake 3.21 以上和 Qt 6.4 以上（推荐 6.8，MSVC 2022 64 位版本）。

在“x64 Native Tools Command Prompt for VS 2022”里：

```
cmake -S windows -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
ctest --test-dir build --output-on-failure
build\app\Compositor.exe
```

打包成可分发的文件夹：

```
cmake --install build --prefix dist\Compositor
windeployqt --release dist\Compositor\Compositor.exe
```

核心库不依赖 Qt，在 Linux 或 macOS 上也能编译和测试：

```
cmake -S windows -B build -G Ninja -DCOMPOSITOR_BUILD_APP=OFF
cmake --build build && ctest --test-dir build
```

## 代码结构

```
windows/
├── core/            核心库（C++17，不依赖 Qt）
│   ├── include/compositor/
│   │   ├── document.h     文档模型，与 manifest.json 一一对应
│   │   ├── project_io.h   .comp 读取、校验、保存
│   │   ├── renderer.h     合成渲染器（直通文件夹、蒙版、剪贴组、调整层）
│   │   ├── blend.h        24 种混合模式
│   │   ├── adjustment.h   12 种调整图层
│   │   ├── edit.h         图层操作、画笔/仿制/修复、渐变、蒙版、画布
│   │   ├── effects.h      图层效果
│   │   ├── selection.h    选区与依赖选区的操作
│   │   └── image_io.h     PNG/JPEG 编解码
│   └── src/
├── app/             Qt 6 界面（主窗口、画布、图层面板、对话框）
├── cli/             命令行工具
├── tests/           核心库单元测试；app/smoke_test.cpp 是界面冒烟测试
├── packaging/       图标、版本资源、Inno Setup 安装脚本
└── third_party/     nlohmann/json、stb_image（单头文件库）
```

C 像素内核不复制，CMake 直接编译 `../Compositor/Rendering/*.c`，两个版本共用同一份代码。

改动保存格式时，要同步更新 `docs/project-format.md`、macOS 版的 `ProjectManifest.current` 和这里的 `kFormatVersion`。
