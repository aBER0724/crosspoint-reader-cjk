# SDカードフォント

[English](./sd-card-fonts.md) | [简体中文](./sd-card-fonts-ZH.md) | [日本語](./sd-card-fonts-JA.md)

CrossPointでは、CJK・キリル文字・ギリシャ文字など、Unicodeを広範囲に収録した追加フォントをSDカードから読み込めます。

ファームウェア容量を抑えるため、内蔵フォントの文字セットは必要な文字を中心に絞っています。収録範囲外の珍しい漢字などは、欠落グリフとして表示される場合があります。より広い CJK 文字範囲が必要な場合は、**設定 > システム > フォント管理**から読書内容に合う **Noto Sans SC**、**Noto Sans TC**、または **Noto Sans JP** をインストールしてください。下記の方法で対応フォントを手動インストールすることもできます。

## フォントのインストール

インストール方法は3通りあります。

### 方法1：端末からダウンロード（推奨）

1. CrossPointリーダーをWi-Fiに接続します
2. **設定 > システム > フォント管理**を開きます
3. 利用可能なフォントファミリーを選び、タップしてダウンロードします
4. ダウンロードしたフォントは、すぐに**設定 > リーダー > フォントファミリー**へ表示されます

### 方法2：Webブラウザーからアップロード

1. **ファイル転送**を開始し、**ネットワークに参加**または**ホットスポットを作成**で接続します
2. リーダーに表示されたWebインターフェースのURLを開きます
3. **フォント**タブへ移動します
4. アップロードフォームから`.cpfont`ファイルをアップロードします

### 方法3：SDカードへ手動コピー

1. [CJKフォントリポジトリ](https://github.com/aBER0724/crosspoint-cjk-fonts)からフォントファイルをダウンロードします
2. フォントファミリーのフォルダーをSDカード上の次のいずれかへコピーします。

   - `/.fonts/` — 隠しディレクトリ（推奨。PCにマウントしたときSDカードのルートを整理された状態に保てます）
   - `/fonts/` — 表示されるディレクトリ（OSがドットファイルを隠す場合や、ファイルマネージャーでフォルダーを確認したい場合に使用します）

   起動時には常に両方のルートがスキャンされ、結果が統合されます。`/.fonts/`が存在していても`/fonts/`内のファミリーは表示され、その逆も同様です。同じファミリー名が両方にある場合のみ競合し、`/.fonts/`側が優先されて`/fonts/`側の重複は無視されます。

       SDカードのルート/
       ├── .fonts/                     ← 隠しルート（推奨）
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← 表示されるルート（同様に利用可能）
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. SDカードを挿入し、CrossPointリーダーの電源を入れます

## ユーザーインターフェースでのCJK表示

本文フォントと UI フォントは個別に選択できます。書名、ファイル名、リスト、画面表示には **設定 > 表示 > UI フォント** で CJK 対応ファミリーを選びます。本文フォントは **設定 > リーダー > フォントファミリー** で設定します。

UI フォントファミリーには 8、10、12 pt のファイルを含めてください。同じファミリーを本文にも使う場合は、変換時に本文用サイズも指定します。

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18,22 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

UI 用の正確なサイズがない場合、CrossPoint は同じファミリー内で最も近いサイズを使います。CJK とラテン文字が混在する文字列は、文字列全体を一つのフォントで描画します。

## ビルド済みフォント

端末のフォントカタログから、メンテナンスされている CJK フォントを閲覧・インストールできます。フォントの入手元、ライセンス、ダウンロード用パッケージは [CrossPoint CJK Fonts リポジトリ](https://github.com/aBER0724/crosspoint-cjk-fonts)で確認できます。手動変換した `.cpfont` ファイルにも対応しています。

## 独自フォントの変換

独自のTrueType/OpenTypeフォントを変換する方法です。

### 前提パッケージ

    pip install freetype-py fonttools

### 単一フォント（1スタイル）

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 14,16,18,22 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### 複数スタイルのフォント

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 14,16,18,22 \
      --name MyFont \
      --output-dir ./MyFont/

### 利用可能なUnicode範囲プリセット

| プリセット | 収録範囲 |
|--------|----------|
| `ascii` | U+0020–U+007E（基本ラテン文字） |
| `latin1` | U+0080–U+00FF（ラテン1補助） |
| `latin-ext` | ヨーロッパ諸言語（ラテン文字 + 拡張A/B + 句読点 + 合字） |
| `greek` | ギリシャ文字 + ギリシャ文字拡張 |
| `cyrillic` | キリル文字 + 補助 |
| `hebrew` | ヘブライ文字 + アルファベット表示形 |
| `georgian` | グルジア文字 + グルジア文字補助 |
| `armenian` | アルメニア文字 |
| `ethiopic` | エチオピア文字 + 拡張 |
| `vietnamese` | ベトナム語サブセット（ơ/ưと結合文字） |
| `punctuation` | 一般句読点（U+2000–U+206F） |
| `cjk` | CJK統合漢字 + ひらがな + カタカナ + 全角文字 |
| `hangul` | ハングル音節 + 字母 + 互換字母 |
| `cherokee` | チェロキー文字（歴史的文字 + 補助ブロック） |
| `tifinagh` | ティフィナグ文字 |
| `symbols` | 数学、通貨、矢印、罫線、各種記号、装飾記号 |
| `reading` | 文芸作品向け：ラテン、ギリシャ、キリル、数学／記号ブロック、補助句読点、CJK引用符 |
| `builtin` | ファームウェア内蔵フォントの変換範囲と同じ |

プリセットはカンマで組み合わせられます：`--intervals latin-ext,greek,cyrillic`

任意のUnicode範囲も直接指定できます：
`--intervals latin-ext,(0x2100-0x214F)`

全プリセットとコードポイント数を表示するには、次を実行します。

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### その他のオプション

`--force-autohint` — フォント固有のヒンティングではなく、FreeTypeの自動ヒンティングを強制します（内蔵ヒントでは小さいサイズの表示品質が悪い場合に有効です）。

独自フォントは、WebインターフェースまたはSDカードへの手動コピーでインストールできます。
