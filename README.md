# M5S3SE SD Browser

**M5Stack CoreS3 SE** に挿した SD カードの内容を、USB 接続した PC の  
Web ブラウザから操作できるファイルブラウザです。

PC 側から操作するための Python スクリプトは [`PC_side_program/README_PC.md`](./PC_side_program/README_PC.md) を参照してください。

---

## 概要

```
PC ─── USB ───┬─ USB NCM (仮想 LAN)  →  http://192.168.7.1/  ブラウザ操作
              └─ USB CDC ACM (シリアル) →  ログ・起動メッセージ受信
```

M5Stack に書き込んだファームウェアが USB 経由で

- **仮想 LAN アダプタ（USB NCM）** を提供し、PC からは `http://192.168.7.1/` でブラウザ操作
- **仮想シリアルポート（USB CDC ACM）** で操作ログ・起動メッセージを出力

SD カードは M5Stack に挿したまま、PC 側から自由に読み書きできます。

---

## Web ブラウザ機能（http://192.168.7.1/）

| ボタン | 機能 |
|--------|------|
| **DIR** | ファイル一覧表示（日時・サイズ）|
| **Upload** | ファイルアップロード（進捗バー付き・タイムスタンプ保持）|
| **Mount** | SD カード再マウント（抜き差し後に使用）|
| **MkDir** | ディレクトリ作成 |
| **Delete** | チェックしたファイル・ディレクトリを削除 |
| **Rename** | チェックしたファイル・ディレクトリの名前を変更 |
| **Set Clock** | PC の現在時刻を M5Stack のシステムクロックに同期 |

- ディレクトリ名クリック → そのディレクトリへ移動
- ファイル名クリック → ダウンロード
- 画面下部に SD カードの Total / Used / Free 容量を表示

---

## LCD 表示

| 表示内容 | 更新タイミング |
|----------|---------------|
| 日付・時刻（シアン） | Set Clock 実行後、1 秒ごとに更新 |
| Total / Used / Free（黄色、KiB 単位） | DIR 実行完了時に更新 |

---

## ハードウェア要件

| 項目 | 内容 |
|------|------|
| ボード | M5Stack CoreS3 SE |
| MCU | ESP32-S3 |
| 接続 | USB-C（PC と直接接続）|
| SD カード | FAT32 フォーマットのもの（⚠ exFAT・NTFS 非対応）|

---

## ビルド環境

| 項目 | バージョン |
|------|-----------|
| ESP-IDF | v5.5.2 |
| espressif/esp_tinyusb | 2.1.0 |
| m5stack/m5unified | 0.2.11 以降 |
| m5stack/m5gfx | 0.2.17 以降 |

---

## ビルド・書き込み手順

1. **ESP-IDF v5.5.2** をインストール

2. リポジトリをクローン
   ```bash
   git clone https://github.com/<your-account>/<repo-name>.git
   cd <repo-name>
   ```

3. 依存コンポーネントを取得
   ```bash
   idf.py update-dependencies
   ```

4. ビルド・書き込み
   ```bash
   idf.py build
   idf.py flash
   ```

> 設定を変更した場合は `sdkconfig` を削除してから `idf.py fullclean && idf.py build` を実行してください。

### sdkconfig.defaults の主要設定

```
CONFIG_TINYUSB_NET_MODE_NCM=y
CONFIG_TINYUSB_CDC_ENABLED=y
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y
CONFIG_FATFS_CODEPAGE_932=y
```

---

## 初回起動手順

1. M5Stack を PC に USB 接続
2. Windows のデバイスマネージャーに以下が現れることを確認
   - **USB イーサネットアダプタ**（USB NCM）
   - **シリアルポート**（USB CDC ACM）
3. ブラウザで `http://192.168.7.1/` を開く
4. **Set Clock** ボタンをクリックして時刻を合わせる
5. SD カードを挿入し、**Mount** をクリック
6. **DIR** でファイル一覧を表示

---

## 制限事項

- **SD カードは FAT32 フォーマットのみ対応**（exFAT・NTFS・FAT16 は非対応）
- SD カード容量は FAT32 の制限（通常 32 GB まで）に従います
- ファイル名は最大 255 文字（LFN 有効時）
- 大文字・小文字を区別しません（FAT32 の仕様）
- 同時複数クライアントからの操作は非対応
- タイムスタンプはローカル時刻で保存されます。**Set Clock** で時刻同期してから使用してください

---

## ファイル構成

```
.
├── main/
│   ├── m5s3se_sd_browser.cpp   # ファームウェア本体
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── Kconfig.projbuild
├── PC_side_program/            # PC 側 Python スクリプト群
│   ├── M5_SDbrowser_PCside.py
│   ├── m5sd_synccopy.py
│   └── README_PC.md            # PC 側プログラムの説明
├── CMakeLists.txt
├── sdkconfig.defaults
├── sdkconfig.defaults.esp32s3
└── README.md                   # このファイル
```

---

## ライセンス

BSD 3-Clause "New" or "Revised" License

