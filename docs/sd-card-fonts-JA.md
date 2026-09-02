# SDカードフォント

[English](./sd-card-fonts.md) | [简体中文](./sd-card-fonts-ZH.md) | [日本語](./sd-card-fonts-JA.md)

CrossPointでは、CJK・キリル文字・ギリシャ文字など、Unicodeを広範囲に収録した追加フォントをSDカードから読み込めます。

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

内蔵UIフォントはラテン文字のみを収録しています。そのため、選択したSDカードフォントで書籍の*本文*を正しく描画できても、インターフェース上の中国語・日本語・韓国語（ライブラリーの書名、ファイルブラウザーのファイル名、リスト項目、見出し）は初期状態では四角い代替文字になります。

大容量のCJKグリフをフラッシュへ収録せずに済むよう、CrossPointは選択済みのSDカードフォントを再利用します。UI文字列に内蔵フォントで描画できないCJK文字が含まれている場合、その文字列全体を選択中のSDカードフォントで描画します。

フォールバックは**同じポイントサイズ**で行われます。内蔵UIフォントは8 pt（小さい文字／著者行）、10 pt（リスト項目）、12 pt（表紙の書名・見出し）で描画されるため、CrossPointはSDフォントファミリーからも同じサイズを読み込み、各UIフォントへ割り当てます。これにより、CJKの書名も周囲のラテン文字と同じ大きさになります。この機能には、読書用の14、16、18、22に加えて、**8、10、12**サイズの`.cpfont`ファイルが必要です。ファミリーに存在しないUIサイズでは、CJK文字は引き続き四角で表示されます。

独自フォントを変換するときは、UI用サイズも指定してください。

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyCJKFont-Regular.otf \
      --intervals cjk \
      --sizes 8,10,12,14,16,18,22 \
      --style regular \
      --name MyCJKFont \
      --output-dir ./MyCJKFont/

実際の動作は次のとおりです。

- **設定 > リーダー > フォントファミリー**でCJK対応のSDフォントを選択します（[フォントのインストール](#フォントのインストール)と、[独自フォントの変換](#独自フォントの変換)にある`cjk` / `hangul`プリセットを参照）。この選択が、書籍本文と、UIでサイズを合わせたCJKフォールバックの両方に使われます。
- ラテン文字だけのUI文字列には鮮明な内蔵フォントが使われます。SDフォントへ切り替わるのは、実際にCJK文字を含む文字列だけです。
- フォールバックはグリフ単位ではなく*文字列*単位です。たとえば`三体 Vol.1`という混在タイトルは、ラテン文字部分を含めてすべてSDフォントで描画されます。SDフォントが`Mono`ファミリーの場合、ラテン文字部分は半角／全角の幅で表示されます。
- SDフォントを選択せず内蔵読書フォントを使用している場合、CJKフォールバックは行われず、UIのCJK文字は再び四角になります。CJK対応のSDフォントを選ぶと元に戻ります。

## ビルド済みフォント

現在のCJKフォント一覧は[CJKフォントリポジトリ](https://github.com/aBER0724/crosspoint-cjk-fonts)で管理されています。この独立リポジトリが再現可能なビルド元兼Release CDNであり、生成されたバイナリーは本ファームウェアリポジトリには保存されません。Actionsワークフローでは、SHA-256で固定したアップストリームソースを検証し、7つの実サイズをビルドしてmanifest schema v2を生成し、`sd-fonts-m2-b4` Releaseを公開します。

ファームウェアは、手動でアップロードした`.cpfont`ファイルにも引き続き対応しています。従来のアップストリーム版ラテンフォントカタログは、元のCrossPointプロジェクトが別途ホストしており、このCJK Releaseには含まれません。

元のカタログ定義は、ファームウェア側の開発と移行用として`lib/EpdFont/scripts/sd-fonts.yaml`に残されています。ソースのダウンロードキャッシュは`downloaded_fonts/`、可変フォントのインスタンスは`instanced_fonts/`、生成した`.cpfont`ファミリーは`output/`に保存されます。これらはローカルのビルド成果物であり、ファームウェアの実行時には使われません。公開された`.cpfont`ファイルは端末の`/.fonts/<Family>/`（推奨）または`/fonts/<Family>/`へインストールされます。

新しいCJKフォントのソースはアップストリームプロジェクトから選定し、リリースまたはコミットに固定します。[jaywcjlove/free-font](https://github.com/jaywcjlove/free-font)のようなカタログは候補探しに利用できますが、アップストリームの再配布ライセンスを確認したフォントだけを追加します。

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
