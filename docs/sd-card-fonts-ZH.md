# SD 卡字体

[English](./sd-card-fonts.md) | [简体中文](./sd-card-fonts-ZH.md) | [日本語](./sd-card-fonts-JA.md)

CrossPoint 支持从 SD 卡加载额外字体，包括覆盖更多 Unicode 字符的字体（中日韩文字、西里尔字母、希腊字母等）。

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

内置 UI 字体只包含拉丁字符，因此默认情况下，即使书籍*正文*能通过所选 SD 卡字体正确显示，界面中的中文、日文或韩文文本（如书库中的书名、文件浏览器中的文件名、列表行和标题）仍会显示为方框。

为了避免在固件闪存中内置庞大的中日韩字形集，CrossPoint 会复用你已选择的 SD 卡字体：当某段 UI 文本含有内置字体无法绘制的中日韩字符时，整段文本都会改用所选 SD 卡字体渲染。

后备字体会**按字号匹配**。内置 UI 字体分别以 8 pt（小字/作者行）、10 pt（列表行）和 12 pt（书籍封面标题、页眉）渲染，因此 CrossPoint 也会加载同一 SD 字体家族的这些字号，并将每种 UI 字体映射到相同字号的 SD 字体。这样，中日韩书名便能与周围的拉丁文字保持相同大小。要使此功能生效，字体家族除阅读字号 14、16、18 和 22 外，还必须包含 **8、10 和 12** 号 `.cpfont` 文件；若缺少某个 UI 字号，该字号下的中日韩字符仍会显示为方框。

转换自己的字体时，请加入 UI 所需字号：

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18,22 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

实际使用时需要注意：

- 在 **设置 > 阅读器 > 字体家族** 中选择支持中日韩字符的 SD 卡字体（参见[安装字体](#安装字体)以及[转换自定义字体](#转换自定义字体)中的 `cjk` / `hangul` 预设）。这一个选择会同时用于书籍正文，以及 UI 中按字号匹配的中日韩后备字体。
- 纯拉丁字符的 UI 文本仍使用清晰的内置字体；只有实际包含中日韩字符的文本才会切换到 SD 卡字体。
- 后备机制以整段*文本*为单位，而不是逐字形处理：例如混合标题 `三体 Vol.1` 会全部使用 SD 卡字体渲染，包括其中的拉丁字符。如果该 SD 字体属于 `Mono` 家族，拉丁字符部分会呈现半角/全角效果。
- 如果未选择 SD 卡字体（当前使用内置阅读字体），则不会启用中日韩后备字体，UI 中的中日韩字符会再次显示为方框；选择支持中日韩字符的 SD 卡字体即可恢复正常显示。

## 可用的预构建字体

当前的中日韩字体列表维护在 [CJK 字体仓库](https://github.com/aBER0724/crosspoint-cjk-fonts)中。该独立仓库是可复现构建的源仓库和 Release CDN；生成的二进制文件不会存放在此固件仓库中。其 Actions 工作流会校验使用 SHA-256 锁定的上游源文件、构建七种实际字号、生成 manifest schema v2，并发布 `sd-fonts-m2-b4` Release。

固件也仍然兼容手动上传的 `.cpfont` 文件。旧版上游拉丁字体目录由原 CrossPoint 项目单独托管，不会混入此 CJK Release。

原始目录定义仍保留在 `lib/EpdFont/scripts/sd-fonts.yaml` 中，用于固件侧开发和迁移。源文件下载缓存位于 `downloaded_fonts/`，可变字体实例位于 `instanced_fonts/`，生成的 `.cpfont` 字体家族位于 `output/`。这些目录都是本地构建产物，固件运行时不会使用。发布的 `.cpfont` 文件会安装到设备上的 `/.fonts/<Family>/`（推荐）或 `/fonts/<Family>/` 中。

新的中日韩字体源从其上游项目中选取，并固定到特定发行版或提交。可以使用 [jaywcjlove/free-font](https://github.com/jaywcjlove/free-font) 之类的目录来发现字体，但只有在确认上游再分发许可证后，字体才会被加入。

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
