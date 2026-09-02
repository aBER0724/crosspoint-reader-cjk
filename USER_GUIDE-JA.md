# CrossPoint ユーザーガイド

**言語：** [English](USER_GUIDE.md) | [简体中文](USER_GUIDE-ZH.md) | [日本語](USER_GUIDE-JA.md)

**CrossPoint** ファームウェアへようこそ。本ガイドでは、端末のハードウェア操作、ナビゲーション、読書機能について説明します。

- [CrossPoint ユーザーガイド](#crosspoint-ユーザーガイド)
  - [1. ハードウェア概要](#1-ハードウェア概要)
    - [ボタン配置](#ボタン配置)
    - [スクリーンショットの撮影](#スクリーンショットの撮影)
  - [2. 電源と起動](#2-電源と起動)
    - [電源のオン／オフ](#電源のオンオフ)
    - [初回起動](#初回起動)
  - [3. 画面](#3-画面)
    - [3.1 ホーム画面](#31-ホーム画面)
    - [3.2 読書モード](#32-読書モード)
    - [3.3 ファイル閲覧画面](#33-ファイル閲覧画面)
    - [3.4 最近の本画面](#34-最近の本画面)
    - [3.5 ファイル転送画面](#35-ファイル転送画面)
    - [3.5.1 Calibre ワイヤレス転送](#351-calibre-ワイヤレス転送)
      - [Calibre へのプラグインのインストール](#calibre-へのプラグインのインストール)
      - [Calibre での CrossPoint プラグインの設定](#calibre-での-crosspoint-プラグインの設定)
      - [本のアップロード](#本のアップロード)
      - [本の削除](#本の削除)
    - [3.6 設定](#36-設定)
      - [3.6.1 表示](#361-表示)
      - [3.6.2 リーダー](#362-リーダー)
      - [3.6.3 操作](#363-操作)
      - [3.6.4 システム](#364-システム)
      - [3.6.5 OPDS サーバー（複数ライブラリ）](#365-opds-サーバー複数ライブラリ)
      - [3.6.6 KOReader 同期クイックセットアップ](#366-koreader-同期クイックセットアップ)
    - [3.7 スリープ画面](#37-スリープ画面)
  - [4. 読書モード](#4-読書モード)
    - [ページめくり](#ページめくり)
    - [章の移動](#章の移動)
    - [システムナビゲーション](#システムナビゲーション)
    - [対応する文字と言語](#対応する文字と言語)
  - [5. 章選択画面](#5-章選択画面)
  - [6. 現在の制限](#6-現在の制限)
  - [7. トラブルシューティングとブートループからの復旧](#7-トラブルシューティングとブートループからの復旧)

## 1. ハードウェア概要

標準では Xteink X4 のボタンを、メーカー製ファームウェアと同じ配置で使用します。

### ボタン配置
| 位置 | ボタン |
| --- | --- |
| **下端** | **戻る**、**決定**、**左**、**右** |
| **右側面** | **電源**、**音量上**、**音量下**、**リセット** |

ボタン配置は **[操作設定](#363-操作)** で変更できます。

### スクリーンショットの撮影

電源ボタンと音量下ボタンを同時に押すと、スクリーンショットが `screenshots/` フォルダーに保存されます。

読書中は **決定** ボタンでリーダーメニューを開き、**Take screenshot** を選択しても撮影できます。

---

## 2. 電源と起動

### 電源のオン／オフ

端末の電源をオンまたはオフにするには、**電源ボタンを約0.5秒長押し**します。
**[操作設定](#363-操作)** では、長押しではなく短押しで電源を切るよう設定できます。

ファームウェア更新後やフリーズ時などに再起動するには、リセットボタンを押して離し、すぐに電源ボタンを数秒間長押しします。

### 初回起動

初めて電源を入れると **[ホーム画面](#31-ホーム画面)** が表示されます。

> [!NOTE]
> 以降の再起動では、最後に読んでいた本が自動的に開きます。

---

## 3. 画面

### 3.1 ホーム画面

ホーム画面はファームウェアの起点です。最後に読んだ本を **[読書モード](#4-読書モード)** で開くほか、**[ファイル閲覧](#33-ファイル閲覧画面)**、**[最近の本](#34-最近の本画面)**、**[ファイル転送](#35-ファイル転送画面)**、**[設定](#36-設定)** へ移動できます。

### 3.2 読書モード

詳しくは後述の[読書モード](#4-読書モード)を参照してください。

### 3.3 ファイル閲覧画面

ファイルやフォルダーを閲覧する画面です。

* **一覧の移動：** **左**（または **音量上**）と **右**（または **音量下**）で、フォルダーや本の選択カーソルを上下に移動します。長押しすると1ページ単位で上下にスクロールできます。
* **選択項目を開く：** **決定** を押すと、フォルダーを開くか、選択した本を読み始めます。
* **ファイルの削除：** **決定** を長押しして離すと、選択したファイルを削除できます。確認画面で削除またはキャンセルを選択します。フォルダーは削除できません。

### 3.4 最近の本画面

最近開いた本を時系列で一覧表示し、書名と著者名を示します。

### 3.5 ファイル転送画面

新しい電子書籍を端末へアップロードする画面です。画面を開くと WiFi 選択ダイアログが表示され、その後 X4 が Web サーバーを起動します。

Web サーバーへの接続方法とファイルのアップロード方法は、[Web サーバーのドキュメント](./docs/webserver.md)を参照してください。

> [!TIP]
> 上級者は `curl` を使い、プログラムまたはコマンドラインからファイルを管理することもできます。詳しくは [Web サーバーのドキュメント](./docs/webserver.md)を参照してください。

### 3.5.1 Calibre ワイヤレス転送

CrossPoint Reader デバイスプラグインを使うと、Calibre から CrossPoint へ本を送信できます。

1. Calibre にプラグインをインストールします。
   - https://github.com/crosspoint-reader/calibre-plugins/releases から最新版の crosspoint_reader プラグインをダウンロードします。
   - zip ファイルをダウンロードします。
   - Calibre → Preferences → Plugins → Load plugin from file → zip ファイルを選択します。
2. 端末で File Transfer → Connect to Calibre → Join a network を選択します。
3. コンピューターが同じ WiFi ネットワークに接続されていることを確認します。
4. Calibre で "Send to device" をクリックして本を転送します。

### 3.6 設定

設定画面では端末の動作を変更できます。

#### 3.6.1 表示

- **Sleep Screen**：スリープ中に表示する画面：
  - "Dark"（既定）— 暗い背景に CrossPoint ロゴを表示する既定のスリープ画面
  - "Light" — 同じ既定画面を白い背景で表示
  - "Custom" — SD カード上のカスタム画像。詳しくは後述の[スリープ画面](#37-スリープ画面)を参照
  - "Cover" — 本の表紙画像（試験的な機能のため、期待どおりに動作しない場合があります）
  - "None" — 空白画面
  - "Cover + Custom" — 本の表紙画像。表示できない場合は "Custom" の動作にフォールバック
- **Sleep Screen Cover Mode**："Cover" 選択時の表紙の表示方法：
  - "Fit"（既定）— 画面中央に収まるよう縮小し、必要に応じて白い余白を追加
  - "Crop" — 画面全体を埋めるよう縮小および切り抜き（試験的な機能のため、期待どおりに動作しない場合があります）
- **Sleep Screen Cover Filter**："Cover" 選択時に表紙へ適用するフィルター：
  - "None"（既定）— グレースケールへ変換してそのまま表示
  - "Contrast" — グレースケール変換せず白黒画像として表示
  - "Inverted" — 白黒を反転し、グレースケール変換せず表示
- **Status Bar**：読書中のステータスバーを設定：
  - "None" — ステータスバーを表示しない
  - "No Progress" — 読書進捗を除くステータスバーを表示
  - "Full w/ Percentage" — 本全体の進捗をパーセントで表示
  - "Full w/ Book Bar" — 本全体の進捗をバーで表示
  - "Book Bar Only" — 本全体の進捗バーのみ表示
  - "Full w/ Chapter Bar" — 章の進捗をバーで表示
- **Hide Battery %**：ステータスバーでバッテリー残量率を隠す範囲を設定します。バッテリーアイコンは引き続き表示されます。
  - "Never"（既定）— 常に残量率を表示
  - "In Reader" — 読書モード以外で残量率を表示
  - "Always" — 常に残量率を非表示
- **Refresh Frequency**：残像を抑えるため、読書中に全画面更新を行う間隔を設定します。1、5、10、15、30ページから選択できます。
- **UI Theme**：使用する UI テーマを設定：
  - "Classic" — 従来の CrossPoint テーマ
  - "Lyra" — 角丸要素とメニューアイコンを採用した新しい CrossPoint テーマ
  - "Lyra Extended" — Lyra と同様ですが、**[ホーム画面](#31-ホーム画面)**に1冊ではなく3冊を表示
- **Sunlight Fading Fix**：白色 X4 を直射日光下で使用すると表示が薄くなる問題に対するソフトウェア修正を設定：
  - "OFF"（既定）— 修正を無効化
  - "ON" — 修正を有効化

#### 3.6.2 リーダー

- **Reader Font Family**：読書用フォントを選択：
  - "Noto Serif"（既定）— Google のセリフ体
  - "Noto Sans" — Google のサンセリフ体
  - "Open Dyslexic" — ディスレクシアの読者向けに設計されたフォント
- **Reader Font Size**：読書時の文字サイズを調整します。"Small"、"Medium"（既定）、"Large"、"X Large" から選択できます。
- **Reader Line Spacing**：行間を調整します。"Tight"、"Normal"（既定）、"Wide" から選択できます。
- **Reader Screen Margin**：読書モードの画面余白を5～40ピクセルの範囲で、5ピクセル単位に設定します。
- **Reader Paragraph Alignment**：段落の配置を設定します。"Justified"（既定）、"Left"、"Center"、"Right" から選択できます。
- **Embedded Style**：EPUB 内の HTML/CSS によるスタイルと書式を使用するかを設定します。"ON" または "OFF" を選択できます。
- **Hyphenation**：読書モードでハイフネーションを行うかを設定します。"ON" または "OFF" を選択できます。
- **Reading Orientation**：EPUB 読書時の画面方向を設定：
  - "Portrait"（既定）— 標準の縦向き
  - "Landscape CW" — 時計回りに回転した横向き
  - "Inverted" — 上下反転した縦向き
  - "Landscape CCW" — 反時計回りに回転した横向き
- **Extra Paragraph Spacing**：段落区切りの処理を設定：
  - "ON" — 読書モードで段落間に縦方向の余白を追加
  - "OFF" — 段落間の余白は追加せず、先頭行を字下げ
- **Text Anti-Aliasing**：読書モードの文字に滑らかな灰色の輪郭（アンチエイリアス）を表示するかを設定します。有効にするとページめくりがわずかに遅くなります。

#### 3.6.3 操作

- **Remap Front Buttons**：下端にある各ボタンの機能を変更するメニューです。
- **Side Button Layout (reader)**：音量上下ボタンの順序を "Prev/Next"（既定）から "Next/Prev" に入れ替えます。読書中のみ有効です。
- **Long-press Chapter Skip**：ページめくりボタンの長押しで前後の章へ移動するかを設定：
  - "Chapter Skip"（既定）— 長押しで前後の章へ移動
  - "Page Scroll" — 長押しで1ページ上下にスクロール
- **Short Power Button Click**：電源ボタンを短押ししたときの動作：
  - "Ignore"（既定）— 電源を切るには長押しが必要
  - "Sleep" — 短押しでスリープモードへ移行
  - "Page Turn" — 読書中の短押しで次ページへ移動し、長押しで電源を切る

#### 3.6.4 システム

- **Time to Sleep**：操作がない状態から自動的にスリープするまでの時間を設定します。1、5、10（既定）、15、30分から選択できます。
- **WiFi Networks**：ファイル転送やファームウェア更新に使用する WiFi ネットワークへ接続します。
- **KOReader Sync**：本の進捗を KOReader と同期するための設定です。
- **OPDS Servers**：本の閲覧やダウンロードに使用する1つ以上の OPDS ライブラリを管理します。後述の [OPDS サーバー（複数ライブラリ）](#365-opds-サーバー複数ライブラリ)を参照してください。
- **Clear Reading Cache**：SD カード内の内部キャッシュを消去します。
- **Check for updates**：WiFi 経由で CrossPoint ファームウェアの更新を確認します。
- **Install firmware from SD**：USB を使わず、SD カード上のファームウェアファイルを書き込みます。
  1. `firmware-sc.bin`、`firmware-tc.bin`、`firmware.bin` のいずれかを SD カードのルートへコピーします。
  2. Settings → System → Install firmware from SD を開きます。
  3. ファイルを確認し、進捗バーが完了するまで待ちます。
  4. 指示が表示されたら再起動します。

  > **警告：** 信頼できるリリースに含まれる有効な X4 OTA アプリイメージのみを使用してください。
  > X3 更新パッケージは使用しないでください。不正なファイルを使うと、USB データ通信が機能しない
  > 端末が復旧不能になるおそれがあります。
- **Language**：画面表示の言語を設定します。

#### 3.6.5 OPDS サーバー（複数ライブラリ）

CrossPoint では複数の OPDS サーバーを保存し、カタログ閲覧時に切り替えられます。

1. **Settings -> System -> OPDS Servers** を開きます。
2. **Add Server** で新規項目を作成するか、既存のサーバーを選んで編集します。
3. 次の項目を設定します。
  - **Server Name**：任意の表示名（例："Home Calibre"、"Public Catalog"）。
  - **OPDS Server URL**：カタログルートの完全な URL。Calibre Content Server では通常 `/opds` で終わります。
  - **Username / Password**：認証が必要なサーバー用の資格情報。任意です。
4. サーバー項目内の **Delete Server** で削除します。

動作上の注意：

- 最大8台の OPDS サーバーを保存できます。
- OPDS 認証は HTTP Basic auth に対応しています。Calibre Content Server で認証を有効にする場合は、Digest ではなく Basic に設定してください。

ファイル転送モードでは、Web インターフェースから OPDS サーバーを管理することもできます。

1. 端末の Web UI に接続します。
2. 端末に表示されるトークン付き設定 URL（例：`http://<device-ip>/settings?token=...`）を開きます。
3. **OPDS Servers** カードで項目を追加、編集、削除します。

#### 3.6.6 KOReader 同期クイックセットアップ

CrossPoint は KOReader 互換同期サーバーと読書進捗を同期できます。同じサーバーと資格情報を使う KOReader アプリや端末とも連携できます。

##### オプション A：CrossPoint 同期サーバー（`sync.crosspointreader.com`、既定）

**Sync Server URL** が空の場合、CrossPoint は `https://sync.crosspointreader.com` の無料同期サーバーを使用します。標準の KOReader 同期プロトコルに対応するため KOReader アプリからも利用でき、さらに正確な spine／ページ位置を保存して CrossPoint 間で欠落のない同期を実現します。

1. 各 CrossPoint 端末で次の操作を行います。

   - **Settings -> System -> KOReader Sync** を開きます。
   - **Username** と **Password** を設定します。平文のパスワードを入力すると CrossPoint が内部で MD5 を計算します。すべての端末で同じ値を使用してください。
   - **Sync Server URL** を空にするか、`https://sync.crosspointreader.com` を設定します。
   - 最初の端末で **Sign Up** を一度実行し、端末から直接アカウントを作成します。ほかの端末では **Authenticate** のみ実行します。

アカウントはサーバーごとに管理されます。既存の `sync.koreader.rocks` 資格情報は CrossPoint サーバーには登録されていません。同じユーザー名とパスワードで再登録するか、オプション B で従来のサーバーを引き続き使用してください。

##### オプション B：従来の公開 KOReader サーバー（`sync.koreader.rocks`）

KOReader 端末を公式公開サーバーとすでに同期している場合に使用します。

1. 各 CrossPoint 端末で次の操作を行います。

   - **Settings -> System -> KOReader Sync** を開きます。
   - **Sync Server URL** に `https://sync.koreader.rocks` を設定します（必須。現在は空欄にすると CrossPoint サーバーが使われます）。
   - **Username** と **Password** に既存の KOReader Sync 資格情報を設定します。
   - **Authenticate** を実行します。

2. アカウントがない場合は、端末で **Sign Up** を実行するか、curl で一度登録します。

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

`HTTP 402` と `{"code":2002,"message":"Username is already registered."}` が返された場合は、別のユーザー名を選ぶか、その既存アカウントを使用します。

2. 各 CrossPoint 端末で次の操作を行います。
   - **Settings -> System -> KOReader Sync** を開きます。
   - **Username** と **Password** を設定します。平文のパスワードを入力すると CrossPoint が内部で MD5 を計算します。すべての端末で同じ値を使用してください。
   - **Sync Server URL** に `https://sync.koreader.rocks` を設定するか、空欄にします。どちらも同じ既定の KOReader 同期サーバーを使用します。
   - **Authenticate** を実行します。

3. 読書中に **決定** を押してリーダーメニューを開き、**Sync Progress** を選択します。
   - **Apply Remote** でリモート側の進捗位置へ移動します。
   - **Upload Local** で現在の進捗をアップロードします。

##### オプション B：セルフホストサーバー（Docker Compose）

1. 同期サーバーを起動します。

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
> `ENABLE_USER_REGISTRATION=true` は初期設定に便利です。ユーザー作成後は、意図しない登録を防ぐため `false` に変更するか削除してください。

2. サーバーを確認します。

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. ユーザーを一度登録します。
CrossPoint は MD5 キーを使って KOReader Sync（`koreader/kosync`）を認証するため、パスワードの MD5 で登録します。

> [!WARNING]
> 再利用可能な MD5 由来のパスワードを平文 HTTP で送るのは安全ではありません。
> 同期専用の一意な資格情報を作成し、主要アカウントのパスワードを再利用しないでください。
> 完全に信頼できる LAN の外へ通信する場合や、信頼できないネットワークでは `https://<server-ip>:7200` を使用してください。
> `curl -k` は自己署名証明書のテストにのみ使用してください。

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

`HTTP 402` と `{"code":2002,"message":"Username is already registered."}` が返された場合、そのアカウントはすでに存在します。

4. 各 CrossPoint 端末で次の操作を行います。
   - **Settings -> System -> KOReader Sync** を開きます。
   - **Username** と **Password** を設定します。平文のパスワードを入力すると CrossPoint が内部で MD5 を計算します。すべての端末で同じ値を使用してください。
   - **Sync Server URL** に `http://<server-ip>:17200` を設定します。
   - **Authenticate** を実行します。

HTTPS リスナーを使う場合は `https://<server-ip>:7200` を指定します。`curl -k` は自己署名証明書のテストにのみ使用してください。

5. 読書中に **決定** を押してリーダーメニューを開き、**Sync Progress** を選択します。
   - **Apply Remote** でリモート側の進捗位置へ移動します。
   - **Upload Local** で現在の進捗をアップロードします。

### 3.7 スリープ画面

**Sleep Screen** 設定は、端末のスリープ中に表示する内容を制御します。

| モード | 動作 |
|------|----------|
| **Dark**（既定） | 暗い背景に CrossPoint ロゴを表示します。 |
| **Light** | 白い背景に CrossPoint ロゴを表示します。 |
| **Custom** | SD カード上のカスタム画像を表示します（下記参照）。画像がない場合は **Dark** にフォールバックします。 |
| **Cover** | 現在開いている本の表紙を表示します。本が開かれていない場合は **Dark** にフォールバックします。 |
| **Cover + Custom** | 現在開いている本の表紙を表示します。本が開かれていない場合は **Custom** の動作にフォールバックします。 |
| **None** | 空白画面を表示します。 |

#### 表紙設定

**Cover** または **Cover + Custom** を使用すると、次の2つの設定も適用されます。

- **Sleep Screen Cover Mode**：**Fit**（白い余白を付けて画面内に収める）または **Crop**（画面全体を埋めるよう拡大・切り抜き）。
- **Sleep Screen Cover Filter**：**None**（グレースケール）、**Contrast**（白黒）、**Inverted**（白黒反転）。

#### カスタム画像

カスタムスリープ画像を使うには、スリープ画面モードを **Custom** または **Cover + Custom** に設定し、SD カードへ画像を配置します。

- **複数画像（推奨）：** SD カードのルートに `.sleep` ディレクトリを作成し、任意の数の `.bmp` 画像を入れます。スリープするたびに1枚がランダムに選択されます。（フォールバックとして `sleep` という名前のディレクトリも使用できます。）
- **単一画像：** ルートディレクトリへ `sleep.bmp` を配置します。`.sleep`/`sleep` ディレクトリに有効な画像がない場合のフォールバックとして使用されます。

> [!TIP]
> 最適な表示のため、次の形式を推奨します。
> - 24ビット色深度の非圧縮 BMP ファイル
> - X4：画面解像度に合わせて 480x800 ピクセル
> - X3：画面解像度に合わせて 528x792 ピクセル

---

## 4. 読書モード

本を開くと、読書しやすいボタン配置に切り替わります。

### ページめくり
| 操作 | ボタン |
| --- | --- |
| **前のページ** | **左** または **音量上** を押す |
| **次のページ** | **右** または **音量下** を押す |

音量（側面）ボタンの役割は **[操作設定](#363-操作)** で入れ替えられます。

**Short Power Button Click** を "Page Turn" に設定すると、電源ボタンの短押しでも次ページへ移動できます。

### 章の移動

* **次の章：** **右**（または **音量下**）を短時間**長押し**して離します。
* **前の章：** **左**（または **音量上**）を短時間**長押し**して離します。

誤って章を移動しないよう、この機能は **[操作設定](#363-操作)** で無効にできます。

脚注を表示中に端末がスリープするか本を閉じた場合、再度開くと脚注ではなく元の読書位置へ戻ります。

### システムナビゲーション

* **ホームへ戻る：** **戻る** ボタンを押すと本を閉じ、**[ホーム画面](#31-ホーム画面)**へ戻ります。
* **ファイル閲覧へ戻る：** **戻る** ボタンを長押しすると本を閉じ、**[ファイル閲覧画面](#33-ファイル閲覧画面)**へ戻ります。
* **章メニュー：** **決定** を押すと**[目次／章選択画面](#5-章選択画面)**が開きます。

### 対応する文字と言語

内蔵フォントはラテン文字とキリル文字に対応します。対応する CJK 本文フォントをインストールまたは選択すれば、中国語、日本語、韓国語の書籍を読めます。アラビア文字やヘブライ文字などの複雑な字形処理には未対応です。

---

## 5. 章選択画面

本を開いているときに **決定** を押すと表示されます。

1. **左**（または **音量上**）と **右**（または **音量下**）で目的の章を選択します。
2. **決定** を押してその章へ移動します。
3. *または **戻る** を押してキャンセルし、現在のページへ戻ります。*

---

## 6. 現在の制限

- 大きな EPUB 表紙からホーム画面やスリープ画面用の画像を生成するには数秒かかることがあります。端末へコピーする前に大きすぎる画像を圧縮すると待ち時間を短縮できます。
- アラビア文字やヘブライ文字などの複雑な字形処理には未対応です。

---

## 7. トラブルシューティングとブートループからの復旧

CrossPoint の使用中に問題やクラッシュが発生した場合は、シリアルモニターのログを添えて Issue を登録してください。端末をコンピューターへ接続し、シリアルモニターを起動するとログを取得できます。[Serial Monitor](https://www.serialmonitor.org/) または次のコマンドを使用できます。

```
pio device monitor
```

端末がブートループから抜けない場合は、リセットボタンを押して離します。次に、設定済みの戻るボタンと電源ボタンを長押ししてホーム画面を起動します。

キャッシュや設定の破損が原因となる場合もあります。その場合は SD カード上の `.crosspoint` ディレクトリを削除してください（または `.crosspoint/` 内の `settings.bin`、`state.bin`、`epub_*` キャッシュディレクトリのみを削除します）。
