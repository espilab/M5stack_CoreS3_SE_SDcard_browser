#
#  M5_SDbrowser_PCside02.py
#
#  M5Stack Core3 SE のSDカード内のファイルをPC側から操作するツール
#
#  2026-4-7 upload/download時に進捗表示を付けた。
#  2026-5-29 ファイル/ディレクトリ名に'+'が含まれる場合のアップロード失敗対策を追加
#
#  Usage:
#    python M5_SDbrowser_PCside01.py l [path]           ファイル一覧
#    python M5_SDbrowser_PCside01.py u <file> [path]    アップロード
#    python M5_SDbrowser_PCside01.py d <remote> [local] ダウンロード

import requests
import json
import sys
import os
import time
import uuid

version = '1.0.2'   # 2026.5.29   "+"対策を追加

# ESP-IDF の httpd は POST ボディも URL デコードするため、
# JSON ボディに含めるパス文字列中の '+' を '%2B' に変換する必要がある。
def penc(path):
    """JSONボディ用パスエンコード: + → %2B（ESP-IDFのURLデコード対策）"""
    return path.replace('+', '%2B')
# Windowsのコマンドライン文字化け対策
if sys.platform == 'win32':
    sys.stdin.reconfigure(encoding='utf-8')
    sys.stdout.reconfigure(encoding='utf-8')



BASE = 'http://192.168.7.1'

# ---- 進捗表示ヘルパー ----
def print_progress(done, total, prefix=''):
    if total > 0:
        pct = done / total * 100
        print(f'\r{prefix}{done:,} / {total:,} B  ({pct:.1f}%)',
              end='', flush=True)
    else:
        print(f'\r{prefix}{done:,} B', end='', flush=True)

def print_progress_done():
    print()


# ---- アップロード用ストリーミングオブジェクト ----
class StreamingMultipart:
    """
    __len__() を持つことで requests が Content-Length モードで送信する。
    (generatorだとchunked encodingになりESP32が切断する)
    """
    def __init__(self, head, local_path, tail, filesize, prefix=''):
        self._head      = head
        self._tail      = tail
        self._filesize  = filesize
        self._prefix    = prefix
        self._total_len = len(head) + filesize + len(tail)
        self._file_obj  = open(local_path, 'rb')
        self._state     = 0   # 0=head  1=file  2=tail  3=done
        self._head_pos  = 0
        self._tail_pos  = 0
        self._file_done = 0

    def __len__(self):
        return self._total_len

    def read(self, size=65536):
        if self._state == 0:            # ヘッダ送信
            chunk = self._head[self._head_pos: self._head_pos + size]
            self._head_pos += len(chunk)
            if self._head_pos >= len(self._head):
                self._state = 1
            return chunk

        elif self._state == 1:          # ファイル本体送信
            chunk = self._file_obj.read(size)
            if chunk:
                self._file_done += len(chunk)
                print_progress(self._file_done, self._filesize, self._prefix)
                return chunk
            else:
                print_progress_done()
                self._state = 2
                return self.read(size)  # tailへ

        elif self._state == 2:          # フッタ送信
            chunk = self._tail[self._tail_pos: self._tail_pos + size]
            self._tail_pos += len(chunk)
            if self._tail_pos >= len(self._tail):
                self._state = 3
            return chunk

        return b''

    def close(self):
        self._file_obj.close()

# ---- ファイル一覧 ----
def list_files(path='/'):
    r = requests.get(f'{BASE}/api/dir', params={'path': path})
    data = r.json()
    files = data.get('files', [])

    # サイズ列の幅をファイル一覧の最大値に合わせる（"サイズ"ヘッダ分は最低6）
    max_size = max((f['size'] for f in files if not f.get('dir')), default=0)
    size_w = max(len(f'{max_size:,}') + 2, 6)  # +2 for " B"

    # ヘッダ
    print(f"{'種別':5s}  {'日付':10s}  {'時刻':8s}  {'サイズ':>{size_w}}  ファイル名")
    print(f"{'-----':5s}  {'----------':10s}  {'--------':8s}  {'-'*size_w}  --------")

    for f in files:
        kind    = '<DIR>' if f.get('dir') else '     '
        date    = f.get('date', '--')
        tm      = f.get('time', '--')
        name    = f['name']
        if f.get('dir'):
            size_str = ''
        else:
            size_str = f"{f['size']:,} B"
        print(f"{kind}  {date:10s}  {tm:8s}  {size_str:>{size_w}}  {name}")

    # 容量情報
    total = data.get('total', 0)
    used  = data.get('used',  0)
    free  = data.get('free',  0)
    if total > 0:
        pct = round(used / total * 100)
        print(f"---")
        print(f"Total: {total:,} B")
        print(f"Used:  {used:,} B ({pct}%)")
        print(f"Free:  {free:,} B")


# ---- ファイルアップロード ----
def upload_file(local_path, remote_dir='/'):
    if not os.path.isfile(local_path):
        print(f"Error: file not found: {local_path}")
        sys.exit(1)

    filename = os.path.basename(local_path)
    filesize = os.path.getsize(local_path)
    print(f"Uploading: {filename} ({filesize:,} B) -> {remote_dir}")

    # タイムスタンプ（ローカル時刻）
    mtime     = int(os.path.getmtime(local_path))
    tz_offset = -time.timezone
    if time.daylight and time.localtime(mtime).tm_isdst:
        tz_offset = -time.altzone
    mtime_local = mtime + tz_offset

    # multipartボディをStreamingMultipartオブジェクトで送信
    # __len__()があるのでrequestsはContent-Lengthモードを使う（chunkedにならない）
    boundary = uuid.uuid4().hex
    part_head = (
        f'--{boundary}\r\n'
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f'Content-Type: application/octet-stream\r\n'
        f'\r\n'
    ).encode('utf-8')
    part_tail = f'\r\n--{boundary}--\r\n'.encode('utf-8')

    body = StreamingMultipart(part_head, local_path, part_tail, filesize, 'Upload ')
    r = requests.post(
        f'{BASE}/api/upload',
        params={'path': remote_dir},
        headers={
            'Content-Type': f'multipart/form-data; boundary={boundary}',
            'X-File-Mtime': str(mtime_local),
        },
        data=body
    )
    body.close()

    if r.status_code == 200 and r.json().get('ok'):
        print(f"Upload OK: {filename}")
    else:
        print(f"Upload FAILED: status={r.status_code}")
        sys.exit(1)


# ---- ファイルダウンロード ----
def download_file(remote_path, local_path=None):
    """
    remote_path : SDカード上のファイルパス（例: /test.txt, /subdir/data.csv）
    local_path  : 保存先ローカルパス（省略時はカレントディレクトリにファイル名で保存）
    """
    # ローカル保存先の決定
    if local_path is None:
        local_path = os.path.basename(remote_path)

    print(f"Downloading: {remote_path} -> {local_path}")

    r = requests.get(
        f'{BASE}/api/download',
        params={'path': remote_path},
        stream=True  # 大きなファイルはストリーミングで受信
    )

    if r.status_code != 200:
        print(f"Download FAILED: status={r.status_code}")
        sys.exit(1)

    total_size  = int(r.headers.get('Content-Length', 0))
    total_bytes = 0
    with open(local_path, 'wb') as f:
        for chunk in r.iter_content(chunk_size=65536):
            f.write(chunk)
            total_bytes += len(chunk)
            print_progress(total_bytes, total_size, 'Download ')
    print_progress_done()

    print(f"Download OK: {local_path} ({total_bytes:,} B)")


# ---- ファイル/ディレクトリ削除 ----
def remove_file(remote_path):
    """
    remote_path : SDカード上のファイルまたはディレクトリのパス
                  複数指定する場合はカンマ区切り（例: /a.txt,/b.txt）
    """
    # カンマ区切りで複数パスを受け付ける
    paths = [p.strip() for p in remote_path.split(',')]

    print(f"Removing: {paths}")

    r = requests.post(
        f'{BASE}/api/delete',
        headers={'Content-Type': 'application/json'},
        json={'paths': paths}
    )

    if r.status_code != 200:
        print(f"Remove FAILED: status={r.status_code}")
        sys.exit(1)

    result = r.json()
    print(f"Remove OK: deleted={result['deleted']}, failed={result['failed']}")
    if result['failed'] > 0:
        print("Warning: some items could not be deleted (directory not empty?)")
        sys.exit(1)


# ---- ディレクトリ作成 ----
def make_dir(remote_path):
    """
    remote_path : 作成するディレクトリのフルパス（例: /newdir, /subdir/newdir）
    """
    print(f"Making dir: {remote_path}")

    r = requests.post(
        f'{BASE}/api/mkdir',
        headers={'Content-Type': 'application/json; charset=utf-8'},
        data=json.dumps({'path': penc(remote_path)}, ensure_ascii=False).encode('utf-8')
    )

    if r.status_code != 200:
        print(f"Mkdir FAILED: status={r.status_code}")
        sys.exit(1)

    result = r.json()
    if result.get('ok'):
        print(f"Mkdir OK: {remote_path}")
    else:
        print(f"Mkdir FAILED: {result.get('error', 'unknown error')}")
        sys.exit(1)


# ---- ESP32に現在時刻をセット ----

# ---- ファイル/ディレクトリ名変更 ----
def rename_file(remote_path, new_name):
    """
    remote_path : 旧ファイルパス（例: /subdir/old.txt）
    new_name    : 新しいファイル名のみ（パス不可、例: new.txt）
    同じディレクトリ内でのリネームのみ行う
    """
    if '/' in new_name:
        print(f"Error: new name must not contain '/': {new_name}")
        sys.exit(1)

    # 同じディレクトリ内で新パスを組み立てる
    dir_part = remote_path.rsplit('/', 1)[0]
    new_path = dir_part + '/' + new_name if dir_part else '/' + new_name

    print(f"Renaming: {remote_path} -> {new_path}")

    r = requests.post(
        f'{BASE}/api/rename',
        headers={'Content-Type': 'application/json; charset=utf-8'},
        data=json.dumps({'from': penc(remote_path), 'to': penc(new_path)},
                        ensure_ascii=False).encode('utf-8')
    )

    if r.status_code != 200:
        print(f"Rename FAILED: status={r.status_code}")
        sys.exit(1)

    result = r.json()
    if result.get('ok'):
        print(f"Rename OK: {new_path}")
    else:
        print(f"Rename FAILED: {result.get('error', 'unknown error')}")
        sys.exit(1)


def set_clock():
    """
    PCの現在時刻をESP32に送信してシステムクロックを合わせる
    FATはローカル時刻で保存するため、ローカル時刻のタイムスタンプを送る
    """
    now       = int(time.time())
    tz_offset = -time.timezone
    if time.daylight and time.localtime().tm_isdst:
        tz_offset = -time.altzone
    t_local = now + tz_offset

    local_str = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime())
    print(f"Setting clock: {local_str}")

    r = requests.get(f'{BASE}/api/setclock', params={'t': str(t_local)})

    if r.status_code != 200:
        print(f"Set clock FAILED: status={r.status_code}")
        sys.exit(1)

    result = r.json()
    if result.get('ok'):
        print(f"Clock set OK: {result.get('time', '')}")
    else:
        print(f"Clock set FAILED: {result.get('error', '')}")
        sys.exit(1)



# ---- ディレクトリを再帰的に削除 ----
def remove_dir_recursive(remote_path):
    """
    remote_path : 削除するディレクトリのフルパス
    ディレクトリ内のファイル・サブディレクトリを再帰的に削除してから
    ディレクトリ自体を削除する
    """

    def collect_all(path):
        """指定パス以下のファイルとディレクトリを再帰的に収集する
        戻り値: (files, dirs)
          files: 削除すべきファイルのフルパスリスト
          dirs : 削除すべきディレクトリのフルパスリスト（深い順）
        """
        r = requests.get(f'{BASE}/api/dir', params={'path': path})
        if r.status_code != 200:
            return [], []
        data = r.json()
        if 'error' in data:
            return [], []

        files = []
        dirs  = []
        for entry in data.get('files', []):
            entry_path = path.rstrip('/') + '/' + entry['name']
            if entry.get('dir'):
                sub_files, sub_dirs = collect_all(entry_path)
                files.extend(sub_files)
                dirs.extend(sub_dirs)
                dirs.append(entry_path)   # 自分自身は子より後に追加
            else:
                files.append(entry_path)
        return files, dirs

    # 1. 対象ディレクトリの存在確認
    print(f"ディレクトリ確認中: {remote_path}")
    r = requests.get(f'{BASE}/api/dir', params={'path': remote_path})
    if r.status_code != 200:
        print(f"Error: アクセス失敗 (status={r.status_code})")
        sys.exit(1)
    data = r.json()
    if 'error' in data:
        print(f"Error: ディレクトリが見つかりません: {remote_path}")
        sys.exit(1)

    # 2. 中身を再帰的に収集
    print("中身を収集中...")
    all_files, all_dirs = collect_all(remote_path)
    total_files = len(all_files)
    total_dirs  = len(all_dirs)

    # 3. 確認プロンプト
    if total_files == 0 and total_dirs == 0:
        msg = f"ディレクトリ {remote_path} は空です。削除しますか? (y/N): "
    else:
        parts = []
        if total_files > 0:
            parts.append(f"ファイル {total_files} 個")
        if total_dirs > 0:
            parts.append(f"サブディレクトリ {total_dirs} 個")
        msg = (f"ディレクトリ {remote_path} を削除します。"
               f"{'、'.join(parts)}が含まれますが、すべて削除します。"
               f"よろしいですか (y/N): ")

    ans = input(msg).strip().lower()
    if ans != 'y':
        print("中止しました。")
        return

    # 4. ファイルを一括削除（API は複数パスを受け付ける）
    ok_cnt = 0
    ng_cnt = 0

    if all_files:
        print(f"ファイルを削除中... ({total_files} 個)")
        # 一度に最大20件ずつAPIに送る
        chunk_size = 20
        for i in range(0, len(all_files), chunk_size):
            chunk = all_files[i:i + chunk_size]
            r = requests.post(
                f'{BASE}/api/delete',
                headers={'Content-Type': 'application/json; charset=utf-8'},
                data=json.dumps({'paths': [penc(p) for p in chunk]}, ensure_ascii=False).encode('utf-8')
            )
            if r.status_code == 200:
                result = r.json()
                ok_cnt += result.get('deleted', 0)
                ng_cnt += result.get('failed',  0)
            else:
                ng_cnt += len(chunk)
        print(f"  ファイル削除: {ok_cnt} 件成功 / {ng_cnt} 件失敗")

    # 5. サブディレクトリを深い順に削除
    if all_dirs:
        print(f"サブディレクトリを削除中... ({total_dirs} 個)")
        for d in all_dirs:   # collect_allが子→親の順に積むのでそのまま使う
            # 削除前に中身を再確認して残存エントリがあれば追加削除
            check_r = requests.get(f'{BASE}/api/dir', params={'path': d})
            if check_r.status_code == 200:
                check_data = check_r.json()
                remaining = [e for e in check_data.get('files', []) if not e.get('dir')]
                if remaining:
                    # 隠しファイル等が残っていたので追加削除
                    extra_paths = [d.rstrip('/') + '/' + e['name'] for e in remaining]
                    print(f"  [残存ファイル削除] {d}: {[e['name'] for e in remaining]}")
                    requests.post(
                        f'{BASE}/api/delete',
                        headers={'Content-Type': 'application/json; charset=utf-8'},
                        data=json.dumps({'paths': [penc(p) for p in extra_paths]}, ensure_ascii=False).encode('utf-8')
                    )
            r = requests.post(
                f'{BASE}/api/delete',
                headers={'Content-Type': 'application/json; charset=utf-8'},
                data=json.dumps({'paths': [penc(d)]}, ensure_ascii=False).encode('utf-8')
            )
            if r.status_code == 200 and r.json().get('deleted', 0) > 0:
                print(f"  削除: {d}")
            else:
                print(f"  失敗: {d}")

    # 6. ルートディレクトリ自体を削除
    print(f"ディレクトリ削除中: {remote_path}")
    r = requests.post(
        f'{BASE}/api/delete',
        headers={'Content-Type': 'application/json; charset=utf-8'},
        data=json.dumps({'paths': [penc(remote_path)]}, ensure_ascii=False).encode('utf-8')
    )
    if r.status_code == 200 and r.json().get('deleted', 0) > 0:
        print(f"削除完了: {remote_path}")
    else:
        print(f"Error: ルートディレクトリの削除に失敗しました: {remote_path}")
        sys.exit(1)


def help():
    global version
    print("Usage:")
    print("  python M5_SDbrowser_PCside01.py l [path]           : ファイル一覧")
    print("  python M5_SDbrowser_PCside01.py u <file> [path]    : アップロード")
    print("  python M5_SDbrowser_PCside01.py d <remote> [local] : ダウンロード")
    print("  python M5_SDbrowser_PCside01.py r <remote>          : 削除")
    print("  python M5_SDbrowser_PCside01.py R <remote_dir>      : ディレクトリを中身ごと再帰削除")
    print("  python M5_SDbrowser_PCside01.py k <remote>          : ディレクトリ作成")
    print("  python M5_SDbrowser_PCside01.py j                   : ESP32の時計をPCの時刻に合わせる")
    print("  python M5_SDbrowser_PCside01.py n <remote> <name>   : ファイル/ディレクトリ名変更")
    print()
    print("Examples:")
    print("  python M5_SDbrowser_PCside01.py l")
    print("  python M5_SDbrowser_PCside01.py l /subdir")
    print("  python M5_SDbrowser_PCside01.py u test.txt")
    print("  python M5_SDbrowser_PCside01.py u test.txt /subdir")
    print("  python M5_SDbrowser_PCside01.py d /test.txt")
    print("  python M5_SDbrowser_PCside01.py d /subdir/data.csv C:/backup/data.csv")
    print("  python M5_SDbrowser_PCside01.py r /old.txt")
    print("  python M5_SDbrowser_PCside01.py r /a.txt,/b.txt,/empty_dir")
    print("  python M5_SDbrowser_PCside01.py k /new_folder")
    print("  python M5_SDbrowser_PCside01.py k /subdir/新しいフォルダ")
    print()
    print('version ',version)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        help()
        sys.exit(1)

    cmd = sys.argv[1]

    if cmd in ('?', 'h', 'help'):
        help()
        sys.exit(0)

    if cmd == 'l':
        path = sys.argv[2] if len(sys.argv) > 2 else '/'
        list_files(path)

    elif cmd == 'u':
        if len(sys.argv) < 3:
            print("Error: upload requires a file path")
            help()
            sys.exit(1)
        local_path = sys.argv[2]
        remote_dir = sys.argv[3] if len(sys.argv) > 3 else '/'
        upload_file(local_path, remote_dir)

    elif cmd == 'd':
        if len(sys.argv) < 3:
            print("Error: download requires a remote file path")
            help()
            sys.exit(1)
        remote_path = sys.argv[2]
        local_path  = sys.argv[3] if len(sys.argv) > 3 else None
        download_file(remote_path, local_path)

    elif cmd == 'r':
        if len(sys.argv) < 3:
            print("Error: remove requires a remote file path")
            help()
            sys.exit(1)
        remote_path = sys.argv[2]
        remove_file(remote_path)

    elif cmd == 'R':
        if len(sys.argv) < 3:
            print("Error: R requires a remote directory path")
            help()
            sys.exit(1)
        remote_path = sys.argv[2]
        remove_dir_recursive(remote_path)

    elif cmd == 'k':
        if len(sys.argv) < 3:
            print("Error: mkdir requires a directory path")
            help()
            sys.exit(1)
        remote_path = sys.argv[2]
        make_dir(remote_path)

    elif cmd == 'j':
        set_clock()

    elif cmd == 'n':
        if len(sys.argv) < 4:
            print("Error: rename requires <remote_path> <new_name>")
            help()
            sys.exit(1)
        remote_path = sys.argv[2]
        new_name    = sys.argv[3]
        rename_file(remote_path, new_name)

    else:
        print(f"Error: unknown command '{cmd}'")
        help()
        sys.exit(1)
