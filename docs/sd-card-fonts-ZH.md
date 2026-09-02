# SD 卡字体

[English](./sd-card-fonts.md) | [简体中文](./sd-card-fonts-ZH.md) | [日本語](./sd-card-fonts-JA.md)

CrossPoint 支持从 SD 卡加载额外字体，包括覆盖更多 Unicode 字符的字体（中日韩文字、西里尔字母、希腊字母等）。

受固件空间限制，内置字体使用了精简字符集。显示字符集以外的生僻字或不常用字符时，可能会出现缺字。需要更完整的中日韩字符覆盖时，建议前往 **设置 > 系统 > 管理字体**，根据阅读内容安装 **Noto Sans SC**、**Noto Sans TC** 或 **Noto Sans JP**；也可以使用下文介绍的手动安装方式。

## 安装字体

有三种安装字体的方式：

### 方式一：通过设备下载（推荐）

1. 将 CrossPoint 阅读器连接到 Wi-Fi
2. 前往 **设置 > 系统 > 管理字体**
3. 浏览可用字体家族，点击即可下载
4. 下载后的字体会立即显示在 **设置 > 阅读器 > 字体家族** 中

### 方式二：通过网页浏览器上传

1. 启动 **文件传输**，然后通过 **加入网络** 或 **创建热点** 建立连接
2. 打开阅读器上显示的 Web 界面地址
3. 进入 **字体** 标签页
4. 使用上传表单上传 `.cpfont` 文件

### 方式三：手动复制到 SD 卡

1. 从 [CJK 字体仓库](https://github.com/aBER0724/crosspoint-cjk-fonts)下载字体文件
2. 将字体家族文件夹复制到 SD 卡上的以下任一位置：

   - `/.fonts/` — 隐藏目录（推荐；在电脑上挂载 SD 卡时可保持根目录整洁）
   - `/fonts/` — 可见目录（如果操作系统会隐藏点号开头的文件，而你希望在文件管理器中看到该文件夹，请使用此目录）

   系统每次启动时都会扫描这两个根目录并合并结果：即使存在 `/.fonts/`，安装在 `/fonts/` 中的字体家族也会正常显示，反之亦然。只有当两个目录中出现同名字体家族时才会发生冲突；此时优先使用 `/.fonts/` 中的版本，并忽略 `/fonts/` 中的重复版本。

       SD 卡根目录/
       ├── .fonts/                     ← 隐藏根目录（推荐）
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← 可见根目录（同样有效）
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. 插入 SD 卡并启动 CrossPoint 阅读器

## 用户界面中的中日韩文字

阅读字体与 UI 字体可独立选择。在 **设置 > 显示 > UI 字体** 中选择支持中日韩字符的字体族，用于书名、文件名、列表和界面文字；正文阅读字体仍在 **设置 > 阅读器 > 字体家族** 中设置。

UI 字体族应包含 8、10 和 12 pt 文件。若同一字体族还用于正文，可在转换时同时加入阅读字号：

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18,22 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

若缺少某个 UI 字号，CrossPoint 会使用同一字体族中最接近的字号。中日韩与拉丁字符混排时，整段文本使用同一种字体渲染。

## 可用的预构建字体

可直接从设备字体目录浏览和安装维护中的中日韩字体。字体来源、许可证和下载包见 [CrossPoint CJK Fonts 仓库](https://github.com/aBER0724/crosspoint-cjk-fonts)。固件也支持手动转换的 `.cpfont` 文件。

## 转换自定义字体

要转换自己的 TrueType/OpenType 字体：

### 前置依赖

    pip install freetype-py fonttools

### 单字体（单一样式）

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 14,16,18,22 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### 多样式字体

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 14,16,18,22 \
      --name MyFont \
      --output-dir ./MyFont/

### 可用的 Unicode 区间预设

| 预设 | 覆盖范围 |
|--------|----------|
| `ascii` | U+0020–U+007E（基本拉丁字母） |
| `latin1` | U+0080–U+00FF（拉丁字母补充-1） |
| `latin-ext` | 欧洲语言（拉丁字母 + 扩展-A/B + 标点 + 连字） |
| `greek` | 希腊字母 + 希腊字母扩展 |
| `cyrillic` | 西里尔字母 + 补充 |
| `hebrew` | 希伯来字母 + 字母表示形式 |
| `georgian` | 格鲁吉亚字母 + 格鲁吉亚字母补充 |
| `armenian` | 亚美尼亚字母 |
| `ethiopic` | 埃塞俄比亚字母 + 扩展 |
| `vietnamese` | 越南语子集（ơ/ư 和组合附加符号） |
| `punctuation` | 通用标点（U+2000–U+206F） |
| `cjk` | 中日韩统一表意文字 + 平假名 + 片假名 + 全角字符 |
| `hangul` | 韩文音节 + 字母 + 兼容字母 |
| `cherokee` | 切罗基文（历史字符 + 补充区块） |
| `tifinagh` | 提非纳文 |
| `symbols` | 数学符号、货币符号、箭头、制表符号、杂项符号和装饰符号 |
| `reading` | 文学作品所需字符：拉丁字母、希腊字母、西里尔字母、数学/符号区块、补充标点和中日韩引号 |
| `builtin` | 与固件内置字体的转换区间一致 |

使用逗号组合多个预设：`--intervals latin-ext,greek,cyrillic`

也可以直接指定任意 Unicode 范围：
`--intervals latin-ext,(0x2100-0x214F)`

列出所有预设及其码位数量：

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### 其他选项

`--force-autohint` — 强制使用 FreeType 自动微调，而不是字体自带的微调信息（适用于字体内置微调在小字号下效果不佳的情况）。

可通过 Web 界面或手动复制到 SD 卡的方式安装自定义字体。
