# PC 側 Python スクリプト

M5Stack SD Browser の PC 側から操作するための Python スクリプトです。  
M5Stack 本体に書き込んだファームウェアが `http://192.168.7.1/` で REST API を提供しており、  
これらのスクリプトはその API を呼び出します。

---

## 必要な環境

- Python 3.9 以降
- `requests` ライブラリ

```bash
pip install requests
```

> Windows に複数の Python がインストールされている場合は、  
> スクリプト名だけでなく `python スクリプト名` の形式で実行してください。

---

## ファイル一覧

| ファイル | 内容 |
|----------|------|
| `M5_SDbrowser_PCside.py` | 個別ファイル操作ツール |
| `m5sd_synccopy.py` | フォルダ一括同期ツール |

---

## M5_SDbrowser_PCside.py

SD カード内のファイルを 1 つずつ操作するコマンドラインツールです。

### 書式

```
python M5_SDbrowser_PCside.py <コマンド> [引数...]
```

### コマンド一覧

| コマンド | 引数 | 動作 |
|---------|------|------|
| `l` | `[path]` | ファイル一覧（省略時はルート `/`）|
| `u` | `<localfile> [path]` | ファイルをアップロード（`path` はアップロード先ディレクトリ、省略時 `/`）|
| `d` | `<remote> [local]` | ファイルをダウンロード（`local` は保存先、省略時はカレントディレクトリ）|
| `r` | `<remote>` | ファイルまたは空ディレクトリを削除（カンマ区切りで複数指定可）|
| `R` | `<remote_dir>` | ディレクトリを中身ごと再帰削除（確認プロンプトあり）|
| `k` | `<remote>` | ディレクトリを作成 |
| `n` | `<remote> <newname>` | ファイル・ディレクトリ名を変更（同一ディレクトリ内のみ）|
| `j` | ―  | PC の現在時刻を M5Stack に同期 |

### 実行例

```bash
# ルートのファイル一覧
python M5_SDbrowser_PCside.py l

# サブディレクトリの一覧
python M5_SDbrowser_PCside.py l /subdir

# ファイルをルートにアップロード
python M5_SDbrowser_PCside.py u data.txt

# ファイルをサブディレクトリにアップロード
python M5_SDbrowser_PCside.py u data.txt /subdir

# ファイルをダウンロード（カレントディレクトリに保存）
python M5_SDbrowser_PCside.py d /data.txt

# ファイルをダウンロード（保存先を指定）
python M5_SDbrowser_PCside.py d /subdir/data.csv C:\backup\data.csv

# ファイルを削除
python M5_SDbrowser_PCside.py r /old.txt

# 複数ファイルを一括削除（カンマ区切り）
python M5_SDbrowser_PCside.py r /a.txt,/b.txt,/empty_dir

# ディレクトリを中身ごと削除
python M5_SDbrowser_PCside.py R /old_folder

# ディレクトリを作成
python M5_SDbrowser_PCside.py k /new_folder

# ファイル名変更
python M5_SDbrowser_PCside.py n /subdir/old.txt new.txt

# 時刻同期
python M5_SDbrowser_PCside.py j
```

### 注意事項

- アップロード・ダウンロードは転送中の進捗（バイト数・パーセント）を表示します
- 日本語パスも対応しています
- タイムスタンプは元ファイルの更新日時を保持してアップロードします  
  （正確な時刻のために先に `j` コマンドで時刻同期を行ってください）
- `r` コマンドで中身のあるディレクトリは削除できません（`R` コマンドを使用）

---

## m5sd_synccopy.py

PC 側のフォルダを M5Stack SD カードへ一括コピー・同期するツールです。  
タイムスタンプが同じファイルはスキップするため、差分転送が可能です。

### 書式

```
python m5sd_synccopy.py <PC側パス> <ESP32側フォルダ> [-d] [-x <MiB>] [-y <MiB>]
```

| 引数 | 説明 |
|------|------|
| `<PC側パス>` | コピー元のローカルフォルダパス |
| `<ESP32側フォルダ>` | コピー先の SD カード上のフォルダパス（`/` 始まり）|
| `-d` | 同期モード：ESP32 側にあって PC 側にないファイル・フォルダを削除 |
| `-x <MiB>` | 上限フィルタ：指定 MiB を**超える**ファイルをスキップ |
| `-y <MiB>` | 下限フィルタ：指定 MiB **以下**のファイルをスキップ |

> PC 側フォルダ名がそのままサブディレクトリとして ESP32 側に作成されます。  
> 例: `C:\data\logs` → `/backup` を指定すると、`/backup/logs/` 以下にコピーされます。  
> PC 側パスがドライブルート（`\`）の場合は `ROOT` というフォルダ名になります。

### サイズフィルタの組み合わせ

| オプション | 効果 |
|-----------|------|
| `-x 100` | 100 MiB 超のファイルをスキップ |
| `-y 10` | 10 MiB 以下のファイルをスキップ |
| `-y 10 -x 100` | 10 MiB 超かつ 100 MiB 以下のファイルのみ転送 |

境界値の扱い：`-y 10` は 10 MiB ちょうどをスキップ、`-x 100` は 100 MiB ちょうどを転送します。

### 実行例

```bash
# 通常コピー（新規・更新ファイルのみ転送）
python m5sd_synccopy.py C:\data\logs /backup

# 同期モード（ESP32 側の余分なファイルも削除）
python m5sd_synccopy.py C:\data\logs /backup -d

# 100 MiB 超のファイルをスキップ
python m5sd_synccopy.py C:\data\logs /backup -x 100

# 10 MiB 超～100 MiB 以下のファイルのみ転送
python m5sd_synccopy.py C:\data\logs /backup -y 10 -x 100

# 全オプション組み合わせ
python m5sd_synccopy.py C:\data\logs /backup -d -y 10 -x 100

# 日本語フォルダも可
python m5sd_synccopy.py C:\写真\旅行 /写真アーカイブ
```

### 動作の流れ

1. ESP32 の時計を PC の現在時刻に同期
2. コピー先フォルダが存在しなければ作成（親フォルダも再帰的に作成、確認済みはキャッシュして重複チェックを省略）
3. PC 側フォルダをサブディレクトリも含めて再帰的に走査
4. サイズフィルタ（`-x`/`-y`）に該当するファイルをスキップ
5. ファイルごとにタイムスタンプを比較し、PC 側が新しい場合のみアップロード
6. `-d` オプション指定時：ESP32 側にあって PC 側にないファイル・ディレクトリを削除

### 表示例

```
コピー元: C:\data\logs  (35 ファイル、サブディレクトリ含む)
コピー先: ESP32:/backup/logs
削除モード: ESP32側の余分なファイル・ディレクトリを削除します
サイズ制限: 10 MiB 超 ～ 100 MiB 以下のみ転送
ESCキーで中断できます

[時刻設定] 2026-04-19 10:30:00
[時刻設定] OK: 2026-04-19 10:30:00

  [DIR] 既存: /backup
  [DIR] 作成: /backup/logs

[1/35] /backup/logs/data.csv  (12,345 B)
         local:2026-04-18 09:00:00  SD:(なし)  -> 新規
  Upload  12,345 / 12,345 B  (100.0%)
  -> OK

[2/35] SKIP(小) /backup/logs/tiny.txt  (512 B <= 10 MiB)

[3/35] SKIP(大) /backup/logs/huge.exe  (922,837,248 B > 100 MiB)

[4/35] SKIP /backup/logs/old.txt
         local:2026-03-01 10:00:00  SD:2026-03-01 10:00:00
...
========================================
完了: 10 件コピー / 25 件スキップ / 0 件失敗 / 35 件中
削除: 2 件削除
```

### ESC キーで中断

実行中に **ESC キー**を押すと確認プロンプトが表示されます。

```
中止しますか? (Y/N):
```

`Y` で中断、`N` で続行します。

---

## REST API リファレンス

ベース URL: `http://192.168.7.1`

Python の `requests` ライブラリや curl などから直接呼び出すこともできます。

### GET /api/dir

ディレクトリの内容と SD カード容量を返します。

```
GET /api/dir?path=/subdir
```

**レスポンス例:**
```json
{
  "path": "/subdir",
  "files": [
    {"name": "data.txt", "date": "2026-04-18", "time": "09:00:00", "size": 1234, "dir": false},
    {"name": "images",   "date": "2026-04-10", "time": "15:00:00", "size": 0,    "dir": true}
  ],
  "total": 15625879552,
  "used":  887980032,
  "free":  14737899520
}
```

### GET /api/download

ファイルをダウンロードします。

```
GET /api/download?path=/subdir/data.txt
```

### POST /api/upload

ファイルをアップロードします。

```
POST /api/upload?path=/subdir
Content-Type: multipart/form-data
X-File-Mtime: <ローカル時刻の Unix タイムスタンプ>
```

**レスポンス:** `{"ok": true}`

### POST /api/delete

ファイルまたは空ディレクトリを削除します。複数パスを一度に指定できます。

```
POST /api/delete
Content-Type: application/json

{"paths": ["/old.txt", "/empty_dir"]}
```

**レスポンス:** `{"deleted": 2, "failed": 0}`

### POST /api/mkdir

ディレクトリを作成します。

```
POST /api/mkdir
Content-Type: application/json

{"path": "/new_folder"}
```

**レスポンス:** `{"ok": true}`

### POST /api/rename

ファイルまたはディレクトリの名前を変更します。

```
POST /api/rename
Content-Type: application/json

{"from": "/subdir/old.txt", "to": "/subdir/new.txt"}
```

**レスポンス:** `{"ok": true}`

### GET /api/mount

SD カードを再マウントします。

```
GET /api/mount
```

**レスポンス:** `{"ok": true, "sd": true}`

### GET /api/setclock

システムクロックを設定します。`t` はローカル時刻の Unix タイムスタンプです。

```
GET /api/setclock?t=1745027400
```

**レスポンス:** `{"ok": true, "time": "2026-04-19 10:30:00"}`

---

## Python からの利用例

```python
import requests
import json
import time

BASE = 'http://192.168.7.1'

# 時刻同期
tz_offset = -time.timezone
t_local = int(time.time()) + tz_offset
requests.get(f'{BASE}/api/setclock', params={'t': str(t_local)})

# ファイル一覧取得
r = requests.get(f'{BASE}/api/dir', params={'path': '/'})
for f in r.json()['files']:
    kind = '<DIR>' if f['dir'] else '     '
    print(f"{kind}  {f['date']} {f['time']}  {f['size']:>12,} B  {f['name']}")

# ファイルアップロード
with open('data.txt', 'rb') as f:
    requests.post(f'{BASE}/api/upload',
                  params={'path': '/'},
                  files={'file': ('data.txt', f, 'application/octet-stream')})

# ファイルダウンロード
r = requests.get(f'{BASE}/api/download', params={'path': '/data.txt'}, stream=True)
with open('data.txt', 'wb') as f:
    for chunk in r.iter_content(chunk_size=65536):
        f.write(chunk)

# ディレクトリ作成（日本語対応）
requests.post(f'{BASE}/api/mkdir',
              headers={'Content-Type': 'application/json; charset=utf-8'},
              data=json.dumps({'path': '/新しいフォルダ'}, ensure_ascii=False).encode('utf-8'))

# ファイル削除（日本語パス対応）
requests.post(f'{BASE}/api/delete',
              headers={'Content-Type': 'application/json; charset=utf-8'},
              data=json.dumps({'paths': ['/不要なファイル.txt']}, ensure_ascii=False).encode('utf-8'))
```
