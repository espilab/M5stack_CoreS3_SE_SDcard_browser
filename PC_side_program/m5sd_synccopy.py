#
#  m5sd_synccopy.py
#
#  PC側のフォルダ内のファイルを M5Stack CoreS3 SE の SDカードに一括コピーする
#
#  Usage:
#    python m5sd_synccopy.py <PC側パス名> <ESP32側フォルダ名>
#
#  Example:
#    python m5sd_synccopy.py C:\data\logs /logs
#    python m5sd_synccopy.py C:\photos /写真フォルダ
#
#  (Note) ESP32側のフォルダ名は / で始まる必要があります。指定がない場合はルートになります。
#        <PC側パス名> のフォルダ名を、  <ESP32側フォルダ名> の中に作成してからコピーする。
#  動作:
#    1. ESP32の時計をPCの時刻に合わせる
#    2. ESP32側のフォルダが無ければ作成する
#    3. PC側フォルダ内のファイルを1つずつアップロードする
#    4. ESCキーで中断確認プロンプトを表示する

import requests
import json
import sys
import os
import time
import uuid
import threading

# Windowsのコマンドライン文字化け対策
if sys.platform == 'win32':
    sys.stdin.reconfigure(encoding='utf-8')
    sys.stdout.reconfigure(encoding='utf-8')

BASE = 'http://192.168.7.1'

# -----------------------------------------------------------------------
# ESCキー検出（バックグラウンドスレッド）
# -----------------------------------------------------------------------
_esc_requested = False   # ESCが押された
_abort_flag    = False   # 中断確定

def _key_watcher():
    """バックグラウンドでキー入力を監視し、ESCを検出したらフラグを立てる"""
    global _esc_requested
    if sys.platform == 'win32':
        import msvcrt
        while not _abort_flag:
            if msvcrt.kbhit():
                ch = msvcrt.getwch()
                if ch == '\x1b':   # ESC
                    _esc_requested = True
            time.sleep(0.05)
    else:
        import tty, termios, select
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            while not _abort_flag:
                if select.select([sys.stdin], [], [], 0.05)[0]:
                    ch = sys.stdin.read(1)
                    if ch == '\x1b':
                        _esc_requested = True
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)

def check_esc():
    """ESCが押されていれば中断確認を行う。中断する場合はTrueを返す"""
    global _esc_requested, _abort_flag
    if not _esc_requested:
        return False
    _esc_requested = False
    print()
    print('\n中止しますか? (Y/N): ', end='', flush=True)
    ans = input().strip().upper()
    if ans == 'Y':
        _abort_flag = True
        return True
    return False


# -----------------------------------------------------------------------
# 進捗表示
# -----------------------------------------------------------------------
def print_progress(done, total, prefix=''):
    if total > 0:
        pct = done / total * 100
        print(f'\r{prefix}{done:,} / {total:,} B  ({pct:.1f}%)',
              end='', flush=True)
    else:
        print(f'\r{prefix}{done:,} B', end='', flush=True)

def print_progress_done():
    print()


# -----------------------------------------------------------------------
# アップロード用ストリーミングオブジェクト
# -----------------------------------------------------------------------
class StreamingMultipart:
    def __init__(self, head, local_path, tail, filesize, prefix=''):
        self._head      = head
        self._tail      = tail
        self._filesize  = filesize
        self._prefix    = prefix
        self._total_len = len(head) + filesize + len(tail)
        self._file_obj  = open(local_path, 'rb')
        self._state     = 0
        self._head_pos  = 0
        self._tail_pos  = 0
        self._file_done = 0

    def __len__(self):
        return self._total_len

    def read(self, size=65536):
        if self._state == 0:
            chunk = self._head[self._head_pos: self._head_pos + size]
            self._head_pos += len(chunk)
            if self._head_pos >= len(self._head):
                self._state = 1
            return chunk
        elif self._state == 1:
            chunk = self._file_obj.read(size)
            if chunk:
                self._file_done += len(chunk)
                print_progress(self._file_done, self._filesize, self._prefix)
                return chunk
            else:
                print_progress_done()
                self._state = 2
                return self.read(size)
        elif self._state == 2:
            chunk = self._tail[self._tail_pos: self._tail_pos + size]
            self._tail_pos += len(chunk)
            if self._tail_pos >= len(self._tail):
                self._state = 3
            return chunk
        return b''

    def close(self):
        self._file_obj.close()


# -----------------------------------------------------------------------
# API関数
# -----------------------------------------------------------------------
def api_set_clock():
    """ESP32の時計をPCの現在時刻に合わせる"""
    now       = int(time.time())
    tz_offset = -time.timezone
    if time.daylight and time.localtime().tm_isdst:
        tz_offset = -time.altzone
    t_local   = now + tz_offset
    local_str = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime())
    print(f'[時刻設定] {local_str}')
    r = requests.get(f'{BASE}/api/setclock', params={'t': str(t_local)}, headers={'Connection':'close'}, timeout=10)
    r.raise_for_status()
    result = r.json()
    if not result.get('ok'):
        raise RuntimeError(f"setclock failed: {result}")
    print(f'[時刻設定] OK: {result.get("time", "")}')


def api_list_dir(path):
    """ESP32側のディレクトリ一覧を取得する。存在しない場合はRuntimeErrorを投げる"""
    r = requests.get(f'{BASE}/api/dir', params={'path': path}, headers={'Connection':'close'}, timeout=10)
    r.raise_for_status()
    data = r.json()
    if 'error' in data:
        raise RuntimeError(data['error'])  # ディレクトリが存在しない
    return data


def api_mkdir(path):
    """ESP32側にディレクトリを作成する"""
    r = requests.post(
        f'{BASE}/api/mkdir',
        headers={'Content-Type': 'application/json; charset=utf-8', 'Connection': 'close'},
        data=json.dumps({'path': path}, ensure_ascii=False).encode('utf-8'),
        timeout=10
    )
    r.raise_for_status()
    result = r.json()
    if not result.get('ok'):
        raise RuntimeError(f"mkdir failed: {result}")


def api_upload(local_path, remote_dir, mtime_local):
    """ファイルをESP32にアップロードする（進捗表示付きストリーミング）"""
    filename = os.path.basename(local_path)
    filesize = os.path.getsize(local_path)

    boundary = uuid.uuid4().hex
    part_head = (
        f'--{boundary}\r\n'
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f'Content-Type: application/octet-stream\r\n'
        f'\r\n'
    ).encode('utf-8')
    part_tail = f'\r\n--{boundary}--\r\n'.encode('utf-8')

    body = StreamingMultipart(part_head, local_path, part_tail, filesize, '  Upload ')
    r = requests.post(
        f'{BASE}/api/upload',
        params={'path': remote_dir},
        headers={
            'Content-Type': f'multipart/form-data; boundary={boundary}',
            'X-File-Mtime': str(mtime_local),
            'Connection':   'close',
        },
        data=body,
        timeout=300
    )
    body.close()
    r.raise_for_status()
    result = r.json()
    if not result.get('ok'):
        raise RuntimeError(f"upload failed: {result}")


# -----------------------------------------------------------------------
# メイン処理
# -----------------------------------------------------------------------
def main():
    global _abort_flag

    if len(sys.argv) < 3:
        print('Usage:')
        print('  python m5sd_synccopy.py <PC側パス名> <ESP32側フォルダ名>')
        print()
        print('Example:')
        print('  python m5sd_synccopy.py C:\\data\\logs /logs')
        sys.exit(1)

    local_dir  = sys.argv[1]
    remote_dir = sys.argv[2]

    # ESP32側パスの先頭は / で始まること
    if not remote_dir.startswith('/'):
        remote_dir = '/' + remote_dir

    # PC側フォルダの存在確認
    if not os.path.isdir(local_dir):
        print(f'Error: local directory not found: {local_dir}')
        sys.exit(1)

    # ESP32側の書き込み先を決定
    # local_dir のフォルダ名をサブディレクトリとして追加する
    # ルート（\ または /）の場合は "ROOT" を使う
    local_basename = os.path.basename(os.path.abspath(local_dir))
    if not local_basename or local_basename in ('\\', '/'):
        local_basename = 'ROOT'
    remote_dir = remote_dir.rstrip('/') + '/' + local_basename

    # os.walk でファイル総数をカウント（表示用）
    all_files = []
    for dirpath, dirnames, filenames in os.walk(local_dir):
        for fn in filenames:
            all_files.append(os.path.join(dirpath, fn))
    if not all_files:
        print(f'No files in {local_dir}')
        sys.exit(0)

    print(f'コピー元: {local_dir}  ({len(all_files)} ファイル、サブディレクトリ含む)')
    print(f'コピー先: ESP32:{remote_dir}')
    print(f'ESCキーで中断できます')
    print()

    # ESCキー監視スレッド起動
    watcher = threading.Thread(target=_key_watcher, daemon=True)
    watcher.start()

    def ensure_remote_dir(path):
        """ESP32側にディレクトリが無ければ、親ディレクトリも含めて再帰的に作成する"""
        # パスを分解して親から順に確認・作成
        parts = [p for p in path.split('/') if p]  # 空要素を除去
        for i in range(1, len(parts) + 1):
            cur = '/' + '/'.join(parts[:i])
            try:
                api_list_dir(cur)
                print(f'  [DIR] 既存: {cur}')
            except Exception:
                print(f'  [DIR] 作成: {cur}')
                api_mkdir(cur)

    def local_to_remote(local_path):
        """ローカルパスをESP32側のパスに変換する"""
        rel = os.path.relpath(local_path, local_dir)
        # Windowsのバックスラッシュをスラッシュに変換
        rel = rel.replace('\\', '/')
        return remote_dir.rstrip('/') + '/' + rel

    try:
        # 1. 時刻設定
        api_set_clock()
        print()

        if check_esc():
            print('中断しました。')
            return

        # 2. ルートフォルダの確認・作成
        ensure_remote_dir(remote_dir)
        print()

        # 3. os.walk でサブディレクトリを含めて再帰的にコピー
        total    = len(all_files)
        ok_cnt   = 0
        ng_cnt   = 0
        skip_cnt = 0
        file_no  = 0

        for dirpath, dirnames, filenames in os.walk(local_dir):
            if _abort_flag:
                break

            # このディレクトリに対応するESP32側パス
            rel_dir = os.path.relpath(dirpath, local_dir).replace('\\', '/')
            if rel_dir == '.':
                cur_remote = remote_dir
            else:
                cur_remote = remote_dir.rstrip('/') + '/' + rel_dir

            # ルート以外はディレクトリを確認・作成
            if rel_dir != '.':
                if check_esc():
                    print('中断しました。')
                    break
                ensure_remote_dir(cur_remote)

            # ESP32側のこのディレクトリのファイル一覧を取得（タイムスタンプ比較用）
            remote_files = {}  # {filename: datetime}
            try:
                dir_data = api_list_dir(cur_remote)
                for entry in dir_data.get('files', []):
                    if not entry.get('dir'):
                        # date="2026-03-29", time="15:39:28" → datetime
                        try:
                            dt_str = f"{entry['date']} {entry['time']}"
                            remote_files[entry['name']] = time.mktime(
                                time.strptime(dt_str, '%Y-%m-%d %H:%M:%S')
                            )
                        except Exception:
                            pass
            except Exception:
                pass  # ディレクトリが空でも続行

            # このディレクトリ内のファイルをアップロード
            for filename in filenames:
                if check_esc():
                    print('中断しました。')
                    break

                file_no += 1
                local_path = os.path.join(dirpath, filename)
                filesize   = os.path.getsize(local_path)

                # ローカルのタイムスタンプ（ローカル時刻）
                mtime     = int(os.path.getmtime(local_path))
                tz_offset = -time.timezone
                if time.daylight and time.localtime(mtime).tm_isdst:
                    tz_offset = -time.altzone
                mtime_local = mtime + tz_offset  # X-File-Mtimeヘッダ用（FATへの書き込み時刻）

                # 表示用ローカル時刻文字列（time.localtime(mtime) で正しい表示になる）
                local_dt = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(mtime))

                # ESP32側に同名ファイルがある場合はタイムスタンプを比較
                # remote_mtimeはtime.mktime()でローカル時刻→unix変換済みなので
                # mtimeと直接比較できる（どちらもUTC基準）
                if filename in remote_files:
                    remote_mtime = remote_files[filename]
                    remote_dt = time.strftime('%Y-%m-%d %H:%M:%S',
                                             time.localtime(remote_mtime))
                    # ローカルが2秒以上新しい場合のみコピー（FAT時刻精度±1秒を考慮）
                    if mtime <= remote_mtime + 2:
                        print(f'[{file_no}/{total}] SKIP {cur_remote}/{filename}')
                        print(f'         local:{local_dt}  SD:{remote_dt}')
                        skip_cnt += 1
                        continue
                    else:
                        print(f'[{file_no}/{total}] {cur_remote}/{filename}  ({filesize:,} B)')
                        print(f'         local:{local_dt}  SD:{remote_dt}  -> 更新')
                else:
                    print(f'[{file_no}/{total}] {cur_remote}/{filename}  ({filesize:,} B)')
                    print(f'         local:{local_dt}  SD:(なし)  -> 新規')
                try:
                    api_upload(local_path, cur_remote, mtime_local)
                    print(f'  -> OK')
                    ok_cnt += 1
                except Exception as e:
                    print(f'  -> FAILED: {e}')
                    ng_cnt += 1
                print()

        # 4. 結果サマリ
        print('=' * 40)
        print(f'完了: {ok_cnt} 件コピー / {skip_cnt} 件スキップ / {ng_cnt} 件失敗 / {total} 件中')

    except KeyboardInterrupt:
        print('\nCtrl+C で中断しました。')
    finally:
        _abort_flag = True


if __name__ == '__main__':
    main()
