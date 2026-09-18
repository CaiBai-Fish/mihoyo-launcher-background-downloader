# dmbg — 米哈游启动器背景资源下载器

从米哈游启动器（`HYP.exe`，基于 Chromium）的磁盘缓存中提取资源 URL，
再从网络下载完整的**背景图片**与**背景视频**。

原始版本是单文件 Python 脚本 `dmbg.py`，现已重构为**零第三方依赖的原生 C++ 程序**：

- HTTP/HTTPS 用 `WinHTTP`（系统自带，TLS 1.2/1.3），不再需要 `requests`
- 目录选择用 `IFileDialog`，不再需要 `tkinter`
- 进度条自己实现，不再需要 `tqdm`
- 产物是单个约 **160 KB** 的 `dmbg.exe`，**不需要安装 Python 或任何运行库**

## 原理

启动器把界面资源留在 Chromium 的缓存索引文件里：

```
%APPDATA%\miHoYo\HYP\1_1\fedata\Cache\Cache_Data\data_1
```

程序读取该文件，用等价于 Python `rb'https?://[!-~]+'` 的字节扫描提取出全部 URL，
筛出背景图/视频，再**从服务器下载完整文件**（不是从缓存块重组）。

## 构建

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载（本仓库用 MSVC 14.51 验证）。

```powershell
# 方式一：直接用 cl.exe（自动通过 vswhere 定位工具链）
.\scripts\build.ps1

# 方式二：走 CMake
cmake -S . -B build -G Ninja
cmake --build build
```

产物：`build\dmbg.exe`

## 用法

```
dmbg [选项]

  -o, --output-dir <目录>   输出根目录（未指定则弹出目录选择框）
  -f, --folder <名称>       输出子文件夹名（默认 downloads_mhy）
  -i, --input <文件>        指定启动器缓存文件
                            默认: %APPDATA%/miHoYo/HYP/1_1/fedata/Cache/Cache_Data/data_1
  -t, --threads <数量>      并发下载线程数（默认 5，范围 1-64）
      --all-exts            下载缓存里所有扩展名，不再只限 webp/webm
      --ext <列表>          自定义扩展名，逗号分隔，如 webp,webm,png
      --force               已存在的文件也重新下载覆盖（默认跳过，避免堆出 _1 副本）
  -ot, --output-txt         把提取到的全部 URL 写入 all_urls.txt
  -h, --help                显示帮助
  -v, --version             显示版本
```

常用组合：

```powershell
dmbg                                  # 弹框选目录，下载全部背景图/视频
dmbg -o D:\bg -f hoyo -t 8            # 指定目录与线程数
dmbg --ext png,jpg -ot -o D:\bg       # 只要图片，并导出 URL 清单
dmbg --all-exts -o D:\all             # 缓存里所有资源
dmbg -o D:\bg --force                 # 强制重新下载，覆盖已有文件
```

### 重复运行是安全的

下载的文件名只由 URL 决定，所以**同一目录重跑不会有任何副作用**：

- 文件已存在且非空 → 跳过，不重新下载，也不会生成 `名字_1.webp` 副本
- 文件是 0 字节（上次被 Ctrl+C 中断留下的空占位）→ 重新下载填满
- 需要强制刷新时用 `--force`，它**就地覆盖**原文件，不会新增副本

> 这一点与 Python 版不同：原版从不覆盖，每重跑一次就会多出一整套 `_1` 副本。

**退出码**：`0` 成功 · `1` 失败（输入缺失、无匹配资源、部分下载失败）· `2` 参数错误

## 与 Python 版的行为差异

保留的部分：URL 提取规则、`webp`/`webm` 默认筛选、文件名生成与去重（`_1`/`_2`）、
100 字符文件名上限、`all_urls.txt`、下载后自动打开输出目录。

改进的部分：

| 项目 | Python 版 | C++ 版 |
|---|---|---|
| 读取被占用的缓存 | 必须先整份复制到 `%TEMP%` | 先尝试共享读取，失败才复制（正常情况省掉一次拷贝，并会清理临时文件） |
| 下载失败的原因 | 静默吞掉，只记一个计数 | 逐条打印具体原因（HTTP 状态码 / WinHTTP 错误） |
| 失败留下的文件 | 会留下 0 字节的空文件 | 失败时连同占位文件一起删除 |
| 缓存分片条目 | 混在"扩展名不匹配"里 | 单独识别并归类（`.webm:2fba...:5` 这类必然 404 的条目） |
| 文件名截断 | 直接切前 100 字符，可能把扩展名切掉 | 保留扩展名 |
| 扩展名判断 | `endswith('.webp')`，`clip.webm?token=x` 会被漏掉 | 先剥离查询串/片段再判断 |
| 长路径 | 受 260 字符限制 | 超长路径自动加 `\\?\` 前缀 |
| 默认路径 | 按 `os.getlogin()` 拼 `C:/Users/<名字>/...` | 用 `%APPDATA%` 环境变量 |
| 重复运行 | **每次都会产生 `_1`、`_2` 副本** | 已存在就跳过，目录不再堆积 |

另外新增 `--all-exts`、`--ext`、`--force` 三个开关。

## 验证

```powershell
.\scripts\verify.ps1
```

该脚本构造一个合成缓存文件（混入真实可下载的 webp/webm、缓存分片条目、
被扩展名过滤项），跑完整流程并断言 44 项：退出码契约、提取数量、筛选结果、
文件魔数（webp 的 `RIFF/WEBP`、webm 的 EBML）、**字节数与服务器 `Content-Length` 一致**、
无空文件与 `.part` 残片、`all_urls.txt` 行数、多级输出目录，以及
**重复运行不产生 `_1` 副本**、0 字节文件会被重新填满、`--force` 就地覆盖。

> 需要网络：会真实拉取约 22 MB 的测试资源。

## 已知限制

- 缓存版本目录 `HYP\1_1` 是写死的（与 Python 版一致）。启动器大版本升级后
  如目录名变化，用 `-i` 指定实际路径。
- 只读取单个缓存索引文件 `data_1`，不遍历整个 `Cache_Data`。
- `--all-exts` 会把 API 端点也当资源尝试下载，部分会 404（属预期，会列在失败明细里）。

## 目录结构

```
src/main.cpp          CLI、文件读取与回退、并发下载、进度条、目录选择框
src/url_scanner.cpp   二进制 URL 提取、扩展名判断、缓存分片识别
src/http_client.cpp   WinHTTP 封装（流式下载、TLS、线程局部连接复用）
src/version.h.in      版本号模板（由 CMake 或 build.ps1 生成 version.h）
scripts/build.ps1     构建脚本（自动定位 MSVC 工具链）
scripts/verify.ps1    端到端验收测试
dmbg.py               原始 Python 版本，保留作参考
```

## 开发注记

几个踩过的坑，改代码时容易再遇到：

- **源文件必须带 UTF-8 BOM。** 本机 `pwsh` 实为 Windows PowerShell 5.1，
  且 MSVC 对无 BOM 的 UTF-8 源码可能按 GBK 解析，中文串尾字节会吞掉后面的
  `%`、引号，报出与真实原因完全无关的错（行号也会错位）。编辑 `.ps1`/`.cpp`
  后请确认 BOM 仍在。
- **版本号走生成头文件，不要用 `/DDMBG_VERSION="x.y.z"`。** PowerShell 构造
  原生命令行时会吃掉内层引号，宏退化成 `1.0.0`，编译报 `std::string + double`。
- **`windows.h` 的 `min`/`max` 宏**会破坏 `std::min`/`std::max`，需定义 `NOMINMAX`。
- **`vswhere -latest` 会漏掉 `isComplete=false` 的安装**（例如刚装完编译器、
  其余组件还在下载）。`build.ps1` 改用 `-all` 并逐个探测 `vcvars64.bat`。
- **PowerShell 数组字面量里的 `'x' * n` 要加括号**：裸写会被解析成数组重复
  运算符，然后拿左边的数组当索引，报 `Cannot convert System.Object[] to System.Int32`。
- **`memcmp` 的长度要用 `sizeof(literal) - 1`**，手写数字极易写错一位；
  曾经把 `"https://"` 按 8 字节比较，导致永远匹配不上。
- 写文件用「先写 `.part` 再 `rename`」，并用 `CreateFile(CREATE_NEW)` 原子占名，
  避免并发下载重名与半成品文件。
