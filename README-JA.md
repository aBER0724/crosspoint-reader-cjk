# CrossPoint Reader CJK

[English](./README.md) · [中文](./README-ZH.md) · **日本語**

**Xteink X4** 向けのコミュニティ製電子ペーパーリーダーファームウェアです。[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) をベースに、中国語や日本語を含む多言語環境で使いやすくなるよう改良しています。

![CrossPoint Reader CJK を搭載した Xteink X4](./docs/images/cover.jpg)

## 主な機能

- 日本語、簡体字中国語、繁体字中国語、英語のユーザーインターフェース
- CJK の組版とフォント表示に対応した EPUB 2/3・TXT リーダー
- SD カードから読み込める本文フォントと UI フォント（`.cpfont` 形式）
- フォントのプレビュー、ダウンロード、インストールに対応した端末内カタログ
- 電子ペーパー向けに調整したライトモードとダークモード。ダークモードでは専用の再描画処理により、背景の白浮きを抑え、表紙画像も正しい階調で表示
- UI 全体の上下反転に対応。端末を逆向きに持った場合も、ホーム画面や設定画面をそのまま操作できます。上流版の上下反転は現在、リーダー画面のみ対応しています
- ファイルブラウザ、表紙表示、スリープ画面、画面回転、読書位置の保存
- Wi-Fi 転送、WebDAV、OTA 更新、Calibre、KOReader Sync

## スクリーンショット

| ホーム・ライト | 設定・ライト |
|:--:|:--:|
| ![日本語のライトモードのホーム画面](./docs/images/current/home-ja-light-0.4.0.png) | ![日本語のライトモードの設定画面](./docs/images/current/settings-ja-light-0.4.0.png) |


## インストール

### Web Flasher

1. Xteink X4 を USB-C で接続します。
2. [CrossPoint CJK Web Flasher](https://xteink-flasher-cjk.vercel.app/) を開きます。
3. **Flash CrossPoint CJK firmware** を選択します。

ファームウェアを手動で入手する場合は、[GitHub Releases](https://github.com/aBER0724/crosspoint-reader-cjk/releases) をご利用ください。

### ソースからビルド

PlatformIO Core、Python 3、サブモジュールを含むソース一式、Xteink X4 が必要です。

```sh
git clone --recursive https://github.com/aBER0724/crosspoint-reader-cjk.git
cd crosspoint-reader-cjk
pio run
pio run --target upload
```

## フォント

端末内のフォントカタログから、本文用・UI 用のフォントファミリーをまとめてインストールできます。フォントは SD カードに保存し、必要なときだけ読み込むことで、ファームウェア容量とメモリ消費を抑えています。

- **[CrossPoint CJK Fonts](https://github.com/aBER0724/crosspoint-cjk-fonts)** — そのままインストールできる `.cpfont` フォントと再現可能なビルド環境
- **[CrossPoint CJK Font Maker](https://github.com/aBER0724/crosspoint-cjk-font-maker)** — 手持ちのフォントから対応パッケージを作成
- **[SD カードフォントガイド](./docs/sd-card-fonts-JA.md)** — 配置方法、フォントリポジトリ、技術仕様

## ドキュメント

- [ユーザーガイド](./USER_GUIDE-JA.md)
- [トラブルシューティング](./docs/troubleshooting-JA.md)
- [Web サーバーガイド](./docs/webserver-JA.md)
- [フォーク固有機能と上流マージ時の保護事項](./docs/fork-features-JA.md)
- [開発者向けドキュメント](./docs/contributing/README-JA.md)
- [プロジェクトの対象範囲](./SCOPE-JA.md)

## コントリビューション

変更内容はできるだけ一つの目的に絞り、Pull Request を作成する前に次のチェックを実行してください。

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

AI コーディングエージェントを使う場合は、[AGENTS.md](./AGENTS.md) も確認してください。

## 謝辞

- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — ベースとなったリーダーファームウェア
- [freeink-sdk](https://github.com/aBER0724/freeink-sdk) — ディスプレイおよびハードウェア SDK
- [CrossPoint CJK Fonts](https://github.com/aBER0724/crosspoint-cjk-fonts) — フォントカタログとビルド環境

CrossPoint Reader CJK は独立したコミュニティプロジェクトです。Xteink および X4 の製造元とは関係ありません。
