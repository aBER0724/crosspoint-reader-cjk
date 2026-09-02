# CrossPoint 用户指南

**语言：** [English](USER_GUIDE.md) | [简体中文](USER_GUIDE-ZH.md) | [日本語](USER_GUIDE-JA.md)

欢迎使用 **CrossPoint** 固件。本指南介绍设备的硬件控制、导航和阅读功能。

- [CrossPoint 用户指南](#crosspoint-用户指南)
  - [1. 硬件概览](#1-硬件概览)
    - [按键布局](#按键布局)
    - [截取屏幕截图](#截取屏幕截图)
  - [2. 电源与启动](#2-电源与启动)
    - [开机/关机](#开机关机)
    - [首次启动](#首次启动)
  - [3. 界面](#3-界面)
    - [3.1 主界面](#31-主界面)
    - [3.2 阅读模式](#32-阅读模式)
    - [3.3 浏览文件界面](#33-浏览文件界面)
    - [3.4 最近阅读界面](#34-最近阅读界面)
    - [3.5 文件传输界面](#35-文件传输界面)
    - [3.5.1 Calibre 无线传输](#351-calibre-无线传输)
      - [在 Calibre 中安装插件](#在-calibre-中安装插件)
      - [在 Calibre 中配置 CrossPoint 插件](#在-calibre-中配置-crosspoint-插件)
      - [上传书籍](#上传书籍)
      - [删除书籍](#删除书籍)
    - [3.6 设置](#36-设置)
      - [3.6.1 显示](#361-显示)
      - [3.6.2 阅读器](#362-阅读器)
      - [3.6.3 控制](#363-控制)
      - [3.6.4 系统](#364-系统)
      - [3.6.5 OPDS 服务器（多个书库）](#365-opds-服务器多个书库)
      - [3.6.6 KOReader 同步快速设置](#366-koreader-同步快速设置)
    - [3.7 休眠画面](#37-休眠画面)
  - [4. 阅读模式](#4-阅读模式)
    - [翻页](#翻页)
    - [章节导航](#章节导航)
    - [系统导航](#系统导航)
    - [支持的语言](#支持的语言)
  - [5. 章节选择界面](#5-章节选择界面)
  - [6. 当前限制与路线图](#6-当前限制与路线图)
  - [7. 问题排查与退出启动循环](#7-问题排查与退出启动循环)

## 1. 硬件概览

设备默认使用 Xteink X4 的标准按键（布局与原厂固件相同）：

### 按键布局
| 位置 | 按键 |
| --- | --- |
| **底边** | **返回**、**确认**、**左**、**右** |
| **右侧** | **电源**、**音量加**、**音量减**、**重置** |

可在 **[控制设置](#363-控制)** 中自定义按键布局。

### 截取屏幕截图

同时按下电源键和音量减键即可截取屏幕截图，并保存到 `screenshots/` 文件夹。

也可以在阅读时按 **确认** 键打开阅读器菜单，然后选择 **Take screenshot**。

---

## 2. 电源与启动

### 开机/关机

要开启或关闭设备，请**按住电源键约半秒**。
在 **[控制设置](#363-控制)** 中，可将电源键配置为短按关机，而非长按关机。

如需重启设备（例如固件更新后或设备死机时），请按下并松开重置键，然后立即按住电源键数秒。

### 首次启动

首次开机后，设备会进入 **[主界面](#31-主界面)**。

> [!NOTE]
> 此后重新启动时，固件会自动重新打开上次阅读的书籍。

---

## 3. 界面

### 3.1 主界面

主界面是固件的主要入口。可在此打开最近阅读书籍并进入 **[阅读模式](#4-阅读模式)**，也可进入 **[浏览文件](#33-浏览文件界面)**、**[最近阅读](#34-最近阅读界面)**、**[文件传输](#35-文件传输界面)** 或 **[设置](#36-设置)**。

### 3.2 阅读模式

详情请参阅下文的[阅读模式](#4-阅读模式)。

### 3.3 浏览文件界面

浏览文件界面用于浏览文件和文件夹。

* **浏览列表：** 使用 **左**（或 **音量加**）和 **右**（或 **音量减**）在文件夹与书籍之间上下移动选择光标。也可长按这些按键向上或向下滚动一整页。
* **打开所选项：** 按 **确认** 键打开文件夹或阅读所选书籍。
* **删除文件：** 按住并松开 **确认** 键可删除所选文件。随后可选择确认或取消删除。不支持删除文件夹。

### 3.4 最近阅读界面

最近阅读界面按时间顺序列出最近打开的书籍，并显示书名和作者。

### 3.5 文件传输界面

文件传输界面用于将新电子书上传至设备。进入该界面后，系统会显示 WiFi 选择对话框，随后 X4 将启动 Web 服务器。

有关连接 Web 服务器及上传文件的方法，请参阅 [Web 服务器文档](./docs/webserver.md)。

> [!TIP]
> 高级用户也可使用 `curl` 以编程方式或通过命令行管理文件。详情请参阅 [Web 服务器文档](./docs/webserver.md)。

### 3.5.1 Calibre 无线传输

CrossPoint 支持通过 CrossPoint Reader 设备插件从 Calibre 发送书籍。

1. 在 Calibre 中安装插件：
   - 前往 https://github.com/crosspoint-reader/calibre-plugins/releases 下载最新版 crosspoint_reader 插件。
   - 下载 zip 文件。
   - 打开 Calibre → Preferences → Plugins → Load plugin from file → 选择该 zip 文件。
2. 在设备上选择：File Transfer → Connect to Calibre → Join a network。
3. 确保电脑连接到同一 WiFi 网络。
4. 在 Calibre 中单击 "Send to device" 传输书籍。

### 3.6 设置

设置界面用于配置设备行为。可调整以下设置：

#### 3.6.1 显示

- **Sleep Screen**：设备休眠时显示的画面：
  - "Dark"（默认）— 默认的深色 CrossPoint 标志休眠画面
  - "Light" — 相同的默认休眠画面，使用白色背景
  - "Custom" — SD 卡中的自定义图像；详情请参阅下文的[休眠画面](#37-休眠画面)
  - "Cover" — 书籍封面图像（注意：此功能尚处于试验阶段，可能无法达到预期效果）
  - "None" — 空白画面
  - "Cover + Custom" — 显示书籍封面；无法显示时回退到 "Custom" 行为
- **Sleep Screen Cover Mode**：选择 "Cover" 休眠画面时书籍封面的显示方式：
  - "Fit"（默认）— 缩小图像并在屏幕中居中显示，必要时添加白边
  - "Crop" — 缩小并按需裁剪图像，以尽量铺满屏幕（注意：此功能尚处于试验阶段，可能无法达到预期效果）
- **Sleep Screen Cover Filter**：选择 "Cover" 休眠画面时应用于书籍封面的滤镜：
  - "None"（默认）— 将封面转换为灰度图并直接显示
  - "Contrast" — 不进行灰度转换，以黑白图像显示
  - "Inverted" — 反转黑白，不进行灰度转换
- **Status Bar**：配置阅读时显示的状态栏：
  - "None" — 不显示状态栏
  - "No Progress" — 显示状态栏，但不显示阅读进度
  - "Full w/ Percentage" — 显示状态栏及书籍进度（百分比）
  - "Full w/ Book Bar" — 显示状态栏及书籍进度（进度条）
  - "Book Bar Only" — 仅显示书籍进度（进度条）
  - "Full w/ Chapter Bar" — 显示状态栏及章节进度（进度条）
- **Hide Battery %**：配置状态栏中隐藏电量百分比的位置；电池图标仍会显示：
  - "Never"（默认）— 始终显示电量百分比
  - "In Reader" — 除阅读模式外均显示电量百分比
  - "Always" — 始终隐藏电量百分比
- **Refresh Frequency**：设置阅读时每隔多少页进行一次全屏刷新以减少残影；可选 1、5、10、15 或 30 页。
- **UI Theme**：设置使用的 UI 主题：
  - "Classic" — 原版 CrossPoint 主题
  - "Lyra" — CrossPoint 新主题，采用圆角元素和菜单图标
  - "Lyra Extended" — Lyra 的扩展版本，在**[主界面](#31-主界面)**显示 3 本书，而不是 1 本
- **Sunlight Fading Fix**：是否启用软件修复，以解决白色 X4 型号在阳光直射下可能褪色的问题：
  - "OFF"（默认）— 禁用修复
  - "ON" — 启用修复

#### 3.6.2 阅读器

- **Reader Font Family**：选择阅读字体：
  - "Noto Serif"（默认）— Google 的衬线字体
  - "Noto Sans" — Google 的无衬线字体
  - "Open Dyslexic" — 为阅读障碍者设计的字体
- **Reader Font Size**：调整阅读文字大小；可选 "Small"、"Medium"（默认）、"Large" 或 "X Large"。
- **Reader Line Spacing**：调整行间距；可选 "Tight"、"Normal"（默认）或 "Wide"。
- **Reader Screen Margin**：控制阅读模式的屏幕边距，范围为 5 至 40 像素，步长为 5 像素。
- **Reader Paragraph Alignment**：设置段落对齐方式；可选 "Justified"（默认）、"Left"、"Center" 或 "Right"。
- **Embedded Style**：是否使用 EPUB 文件内嵌的 HTML 和 CSS 样式及格式；可选 "ON" 或 "OFF"。
- **Hyphenation**：是否在阅读模式中对文本进行断词；可选 "ON" 或 "OFF"。
- **Reading Orientation**：设置阅读 EPUB 文件时的屏幕方向：
  - "Portrait"（默认）— 标准竖屏方向
  - "Landscape CW" — 顺时针旋转的横屏方向
  - "Inverted" — 上下颠倒的竖屏方向
  - "Landscape CCW" — 逆时针旋转的横屏方向
- **Extra Paragraph Spacing**：设置段落分隔方式：
  - "ON" — 在阅读模式中增加段落间的垂直间距
  - "OFF" — 不增加段落间距，但使用首行缩进
- **Text Anti-Aliasing**：是否在阅读模式中为文字显示平滑的灰色边缘（抗锯齿）。请注意，这会使翻页速度略微变慢。

#### 3.6.3 控制

- **Remap Front Buttons**：用于自定义各底边按键功能的菜单。
- **Side Button Layout (reader)**：将音量加减键的顺序从 "Prev/Next"（默认）交换为 "Next/Prev"。此更改仅在阅读时生效。
- **Long-press Chapter Skip**：设置长按翻页键是否跳至上一章或下一章：
  - "Chapter Skip"（默认）— 长按时跳至上一章或下一章
  - "Page Scroll" — 长按时向上或向下滚动一页
- **Short Power Button Click**：控制短按电源键的效果：
  - "Ignore"（默认）— 必须长按才能关闭设备
  - "Sleep" — 短按使设备进入休眠模式
  - "Page Turn" — 在阅读模式中短按翻到下一页；长按关闭设备

#### 3.6.4 系统

- **Time to Sleep**：设置设备无操作后自动休眠的时间；可选 1、5、10（默认）、15 或 30 分钟。
- **WiFi Networks**：连接 WiFi 网络以传输文件和更新固件。
- **KOReader Sync**：设置 KOReader 书籍进度同步。
- **OPDS Servers**：管理一个或多个用于浏览和下载书籍的 OPDS 书库。请参阅下文的 [OPDS 服务器（多个书库）](#365-opds-服务器多个书库)。
- **Clear Reading Cache**：清除 SD 卡中的内部缓存。
- **Check for updates**：通过 WiFi 检查 CrossPoint 固件更新。
- **Install firmware from SD**：无需 USB，直接烧录 SD 卡中的固件文件。
  1. 将 `firmware-sc.bin`、`firmware-tc.bin` 或 `firmware.bin` 复制到 SD 卡根目录。
  2. 打开 Settings → System → Install firmware from SD。
  3. 确认文件并等待进度条完成。
  4. 出现提示时重新启动。

  > **警告：** 仅使用可信发布版本中有效的 X4 OTA 应用程序镜像。
  > 请勿使用 X3 更新包。错误文件可能使没有可用 USB 数据连接的
  > 设备永久变砖。
- **Language**：设置系统语言（详情请参阅**[支持的语言](#支持的语言)**）。

#### 3.6.5 OPDS 服务器（多个书库）

CrossPoint 支持保存多个 OPDS 服务器，并可在浏览目录时切换服务器。

1. 打开 **Settings -> System -> OPDS Servers**。
2. 选择 **Add Server** 新建条目，或选择现有服务器进行编辑。
3. 配置以下字段：
  - **Server Name**：可选的显示名称（例如 "Home Calibre" 或 "Public Catalog"）。
  - **OPDS Server URL**：完整的目录根 URL（Calibre Content Server 通常以 `/opds` 结尾）。
  - **Username / Password**：用于需要身份验证的服务器，可选。
4. 在服务器条目中使用 **Delete Server** 将其删除。

行为说明：

- 最多可保存 8 个 OPDS 服务器。
- OPDS 身份验证支持 HTTP Basic auth。如果启用了 Calibre Content Server 身份验证，请将其设置为 Basic，而不是 Digest。

在文件传输模式下，也可通过 Web 界面管理 OPDS 服务器：

1. 连接设备的 Web UI。
2. 打开设备上显示的带令牌设置 URL，例如 `http://<device-ip>/settings?token=...`。
3. 使用 **OPDS Servers** 卡片添加、编辑或删除条目。

#### 3.6.6 KOReader 同步快速设置

CrossPoint 可与兼容 KOReader 的同步服务器同步阅读进度。KOReader 应用或设备使用相同服务器及凭据时，也可与其互通。

##### 方案 A：CrossPoint 同步服务器（`sync.crosspointreader.com`，默认）

**Sync Server URL** 留空时，CrossPoint 使用位于 `https://sync.crosspointreader.com` 的免费 CrossPoint 同步服务器。该服务器使用标准 KOReader 同步协议（因此 KOReader 应用也可使用），并额外保存精确的 spine/页面位置，以实现 CrossPoint 设备间的无损同步。

1. 在每台 CrossPoint 设备上：

   - 前往 **Settings -> System -> KOReader Sync**。
   - 设置 **Username** 和 **Password**（输入明文密码；CrossPoint 会在内部计算 MD5，所有设备应使用相同值）。
   - 将 **Sync Server URL** 留空（或设为 `https://sync.crosspointreader.com`）。
   - 在第一台设备上运行一次 **Sign Up**，直接通过设备创建账户。其他设备只需运行 **Authenticate**。

账户按服务器区分。现有 `sync.koreader.rocks` 凭据并不存在于 CrossPoint 服务器上；可使用相同用户名和密码重新注册，或使用方案 B 继续使用旧服务器。

##### 方案 B：旧版公共 KOReader 服务器（`sync.koreader.rocks`）

如果 KOReader 设备已通过官方公共服务器进行同步，请使用此方案。

1. 在每台 CrossPoint 设备上：

   - 前往 **Settings -> System -> KOReader Sync**。
   - 将 **Sync Server URL** 设为 `https://sync.koreader.rocks`（必须设置；现在留空会改用 CrossPoint 服务器）。
   - 将 **Username** 和 **Password** 设为现有 KOReader Sync 凭据。
   - 运行 **Authenticate**。

2. 如果尚无账户，请在设备上运行 **Sign Up**，或使用 curl 注册一次：

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

如果返回 `HTTP 402` 和 `{"code":2002,"message":"Username is already registered."}`，请选择其他用户名，或使用该现有账户。

2. 在每台 CrossPoint 设备上：
   - 前往 **Settings -> System -> KOReader Sync**。
   - 设置 **Username** 和 **Password**（输入明文密码；CrossPoint 会在内部计算 MD5，所有设备应使用相同值）。
   - 将 **Sync Server URL** 设为 `https://sync.koreader.rocks`，或留空（两者都使用同一个默认 KOReader 同步服务器）。
   - 运行 **Authenticate**。

3. 阅读时按 **确认** 打开阅读器菜单，然后选择 **Sync Progress**。
   - 选择 **Apply Remote** 跳至远程进度。
   - 选择 **Upload Local** 上传当前进度。

##### 方案 B：自托管服务器（Docker Compose）

1. 启动同步服务器：

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> `ENABLE_USER_REGISTRATION=true` 便于首次设置。创建用户后，请将其设为 `false`（或删除该项），以避免意外注册。

2. 验证服务器：

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. 注册一次用户。
CrossPoint 使用 MD5 密钥向 KOReader Sync（`koreader/kosync`）进行身份验证，因此注册时应使用密码的 MD5：

> [!WARNING]
> 通过明文 HTTP 发送可重复使用的 MD5 衍生密码并不安全。
> 请创建专用于同步的唯一凭据，不要重复使用主账户密码。
> 流量离开完全可信的局域网或使用不可信网络时，优先使用 `https://<server-ip>:7200`。
> `curl -k` 仅用于测试自签名证书。

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

如果返回 `HTTP 402` 和 `{"code":2002,"message":"Username is already registered."}`，则该账户已存在。

4. 在每台 CrossPoint 设备上：
   - 前往 **Settings -> System -> KOReader Sync**。
   - 设置 **Username** 和 **Password**（输入明文密码；CrossPoint 会在内部计算 MD5，所有设备应使用相同值）。
   - 将 **Sync Server URL** 设为 `http://<server-ip>:17200`。
   - 运行 **Authenticate**。

如果使用 HTTPS 监听器，请使用 `https://<server-ip>:7200`（`curl -k` 仅用于测试自签名证书）。

5. 阅读时按 **确认** 打开阅读器菜单，然后选择 **Sync Progress**。
   - 选择 **Apply Remote** 跳至远程进度。
   - 选择 **Upload Local** 上传当前进度。

### 3.7 休眠画面

**Sleep Screen** 设置控制设备休眠时显示的内容：

| 模式 | 行为 |
|------|----------|
| **Dark**（默认） | 深色背景上的 CrossPoint 标志。 |
| **Light** | 白色背景上的 CrossPoint 标志。 |
| **Custom** | SD 卡中的自定义图像（见下文）。如果找不到自定义图像，则回退到 **Dark**。 |
| **Cover** | 当前打开书籍的封面。如果未打开书籍，则回退到 **Dark**。 |
| **Cover + Custom** | 当前打开书籍的封面。如果未打开书籍，则回退到 **Custom** 行为。 |
| **None** | 空白画面。 |

#### 封面设置

使用 **Cover** 或 **Cover + Custom** 时，还会应用以下两项设置：

- **Sleep Screen Cover Mode**：**Fit**（缩放以适应屏幕并添加白边）或 **Crop**（缩放并裁剪以铺满屏幕）。
- **Sleep Screen Cover Filter**：**None**（灰度）、**Contrast**（黑白）或 **Inverted**（反转黑白）。

#### 自定义图像

要使用自定义休眠图像，请将休眠画面模式设为 **Custom** 或 **Cover + Custom**，然后将图像放入 SD 卡：

- **多张图像（推荐）：** 在 SD 卡根目录创建 `.sleep` 目录，并放入任意数量的 `.bmp` 图像。设备每次休眠时会随机选择一张。（也可使用名为 `sleep` 的目录作为回退。）
- **单张图像：** 在根目录放置名为 `sleep.bmp` 的文件。如果 `.sleep`/`sleep` 目录中没有有效图像，则使用此文件作为回退。

> [!TIP]
> 为获得最佳效果：
> - 使用 24 位色深的未压缩 BMP 文件
> - X4：使用 480x800 像素分辨率，以匹配设备屏幕分辨率。
> - X3：使用 528x792 像素分辨率，以匹配设备屏幕分辨率。

---

## 4. 阅读模式

打开书籍后，按键布局会改变，以便于阅读。

### 翻页
| 操作 | 按键 |
| --- | --- |
| **上一页** | 按 **左** 或 **音量加** |
| **下一页** | 按 **右** 或 **音量减** |

可在 **[控制设置](#363-控制)** 中交换音量（侧边）按键的作用。

如果 **Short Power Button Click** 设置为 "Page Turn"，也可短按电源键翻到下一页。

### 章节导航

* **下一章：** 短暂**按住** **右**（或 **音量减**）键，然后松开。
* **上一章：** 短暂**按住** **左**（或 **音量加**）键，然后松开。

可在 **[控制设置](#363-控制)** 中禁用此功能，以避免误切换章节。

如果设备在查看脚注时进入休眠，或此时关闭书籍，再次打开时会返回原阅读位置，而不是脚注位置。

### 系统导航

* **返回主界面：** 按 **返回** 键关闭书籍并返回**[主界面](#31-主界面)**。
* **返回浏览文件：** 按住 **返回** 键关闭书籍并返回**[浏览文件](#33-浏览文件界面)**界面。
* **章节菜单：** 按 **确认** 键打开**[目录/章节选择](#5-章节选择界面)**界面。

### 支持的语言

CrossPoint 使用以下 Unicode 字符区块渲染文字，因此支持多种语言：

* **拉丁字母（基本、补充、扩展 A）：** 涵盖英语、德语、法语、西班牙语、葡萄牙语、意大利语、荷兰语、瑞典语、挪威语、丹麦语、芬兰语、波兰语、捷克语、匈牙利语、罗马尼亚语、斯洛伐克语、斯洛文尼亚语、土耳其语等。
* **西里尔字母（标准和扩展）：** 涵盖俄语、乌克兰语、白俄罗斯语、保加利亚语、塞尔维亚语、马其顿语、哈萨克语、吉尔吉斯语、蒙古语等。

不支持的语言：中文、日语、韩语、越南语、希伯来语、阿拉伯语、希腊语和波斯语。

### 支持的语言

CrossPoint 使用以下 Unicode 字符区块渲染文字，因此支持多种语言：

* **拉丁字母（基本、补充、扩展 A）：** 涵盖英语、德语、法语、西班牙语、葡萄牙语、意大利语、荷兰语、瑞典语、挪威语、丹麦语、芬兰语、波兰语、捷克语、匈牙利语、罗马尼亚语、斯洛伐克语、斯洛文尼亚语、土耳其语等。
* **西里尔字母（标准和扩展）：** 涵盖俄语、乌克兰语、白俄罗斯语、保加利亚语、塞尔维亚语、马其顿语、哈萨克语、吉尔吉斯语、蒙古语等。

不支持的语言：中文、日语、韩语、越南语、希伯来语、阿拉伯语、希腊语和波斯语。

---

## 5. 章节选择界面

在书籍中按 **确认** 键即可打开。

1. 使用 **左**（或 **音量加**）和 **右**（或 **音量减**）突出显示所需章节。
2. 按 **确认** 键跳至该章节。
3. *也可按 **返回** 键取消并返回当前页面。*

---

## 6. 当前限制与路线图

请注意，此固件目前仍在积极开发中。以下功能**尚不支持**，但计划在未来更新中加入：

* **图像：** 不会渲染电子书中的内嵌图像。
* **封面图像：** 将 EPUB 中内嵌的大型封面转换为休眠画面及主界面缩略图需要数秒（高度约 2000 像素的图像约需 10 秒）。可考虑使用 https://github.com/bigbag/epub-to-xtc-converter 等工具优化 EPUB，以加快处理速度。

---

## 7. 问题排查与退出启动循环

使用 CrossPoint 时如遇问题或崩溃，欢迎提交问题工单并附上串口监视器日志。将设备连接到电脑并启动串口监视器即可获取日志。可使用 [Serial Monitor](https://www.serialmonitor.org/) 或以下命令：

```
pio device monitor
```

如果设备陷入启动循环，请按下并松开重置键。然后按住所配置的返回键和电源键，启动到主界面。

缓存或配置损坏也可能导致问题。此时请删除 SD 卡上的 `.crosspoint` 目录（也可考虑仅删除 `.crosspoint/` 文件夹中的 `settings.bin`、`state.bin` 或 `epub_*` 缓存目录）。
