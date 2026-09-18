import os
import re
import sys
import shutil
import tempfile
import requests
import threading
import argparse
from urllib.parse import urlparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from tqdm import tqdm
import ctypes

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(1)  # Windows 8.1+
except:
    pass


# ==================== 配置项 ====================
ALLOWED_EXTS = {'webp', 'webm'}
DEFAULT_TIMEOUT = 30
CHUNK_SIZE = 8192
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"

try:
    CURRENT_USERNAME = os.getlogin()
except OSError:
    CURRENT_USERNAME = os.environ.get('USERNAME', 'DefaultUser')

DEFAULT_INPUT_PATH = f"C:/Users/{CURRENT_USERNAME}/AppData/Roaming/miHoYo/HYP/1_1/fedata/Cache/Cache_Data/data_1"
# ===============================================


def copy_to_temp(input_file):
    """复制文件到系统临时目录"""
    try:
        temp_dir = tempfile.mkdtemp(prefix="dmbg_")
        temp_path = os.path.join(temp_dir, os.path.basename(input_file))
        shutil.copy2(input_file, temp_path)
        print(f"已复制文件到临时目录: {temp_path}")
        return temp_path, temp_dir
    except Exception as e:
        print(f"无法复制文件到临时目录: {e}")
        sys.exit(1)


def extract_http_urls_from_binary(input_file):
    """使用正则表达式快速提取HTTP/HTTPS URL"""
    try:
        with open(input_file, 'rb') as f:
            data = f.read()

        urls = re.findall(rb'https?://[!-~]+', data)
        decoded_urls = []
        for raw in urls:
            try:
                decoded = raw.decode('ascii')
                if len(decoded) > 10:
                    decoded_urls.append(decoded)
            except UnicodeDecodeError:
                continue

        return sorted(set(decoded_urls))

    except Exception as e:
        print(f"提取URL时发生错误: {e}")
        sys.exit(1)


def generate_safe_filename(url, response=None):
    """生成安全文件名"""
    parsed = urlparse(url)
    filename = os.path.basename(parsed.path)
    if not filename or '.' not in filename:
        filename = f"{parsed.hostname}_{abs(hash(url)) % 10000}"

    filename = "".join(c for c in filename if c.isalnum() or c in "._-")

    if response:
        content_type = response.headers.get('content-type', '').lower()
        if '.' not in filename:
            if 'html' in content_type:
                filename += '.html'
            elif 'json' in content_type:
                filename += '.json'
            elif 'image' in content_type:
                ext = content_type.split('/')[-1].split(';')[0]
                filename += f'.{ext}'
            else:
                filename += '.bin'

    return filename[:100]


def download_url(session, url, output_dir, pbar, stats, lock):
    """下载单个URL"""
    try:
        if not any(url.lower().endswith(f".{ext}") for ext in ALLOWED_EXTS):
            with lock:
                stats['skipped'] += 1
            pbar.update(1)
            return

        resp = session.get(url, timeout=DEFAULT_TIMEOUT, stream=True)
        resp.raise_for_status()

        filename = generate_safe_filename(url, resp)
        filepath = os.path.join(output_dir, filename)

        base, ext = os.path.splitext(filepath)
        counter = 1
        while os.path.exists(filepath):
            filepath = f"{base}_{counter}{ext}"
            counter += 1

        with open(filepath, 'wb') as f:
            for chunk in resp.iter_content(CHUNK_SIZE):
                f.write(chunk)

        with lock:
            stats['success'] += 1
        pbar.update(1)

    except Exception:
        with lock:
            stats['failed'] += 1
        pbar.update(1)


def choose_output_directory():
    """手动选择输出目录（修改默认路径）"""
    try:
        import tkinter
        from tkinter import filedialog
    except ImportError:
        print("无法导入tkinter，请安装带Tk的Python。")
        sys.exit(1)

    root = tkinter.Tk()
    root.withdraw()  # 隐藏主窗口
    root.attributes('-topmost', True)  # 窗口置顶

    default_initial_dir = os.path.expanduser("~/Downloads")  # 默认打开下载目录

    # 打开目录选择对话框（指定默认路径）
    output = filedialog.askdirectory(
        title="选择下载目录",
        initialdir=default_initial_dir  # 设置默认路径
    )

    root.destroy()

    if not output:
        print("用户取消目录选择。")
        sys.exit(0)
    return output


def main():
    parser = argparse.ArgumentParser(
        description=f"从米哈游启动器缓存文件提取并下载背景资源（仅限 {', '.join(ALLOWED_EXTS)}）"
    )
    parser.add_argument('-f', '--folder', help='输出子文件夹名')
    parser.add_argument('-o', '--output-dir', help='输出根目录（若未指定则弹出选择框）')
    parser.add_argument('-t', '--threads', type=int, default=5, help='并发线程数')
    parser.add_argument('-ot', '--output-txt', action='store_true', help='保存所有URL到文本')

    args = parser.parse_args()

    # 检查输入文件
    input_path = os.path.expandvars(DEFAULT_INPUT_PATH)
    if not os.path.exists(input_path):
        print(f"输入文件不存在: {input_path}")
        print("请确认已运行启动器或文件路径正确。")
        sys.exit(1)

    # 使用命令行指定或弹出选择
    if args.output_dir:
        output_root = os.path.abspath(args.output_dir)
        print(f"使用命令行指定输出目录: {output_root}")
        os.makedirs(output_root, exist_ok=True)
    else:
        print("请选择输出目录...")
        output_root = choose_output_directory()

    folder_name = args.folder or "downloads_mhy"
    output_dir = os.path.join(output_root, folder_name)
    os.makedirs(output_dir, exist_ok=True)

    # === 复制文件到临时目录 ===
    temp_path, temp_dir = copy_to_temp(input_path)

    print("正在提取URL...")
    urls = extract_http_urls_from_binary(temp_path)
    if not urls:
        print("未提取到有效URL")
        shutil.rmtree(temp_dir, ignore_errors=True)
        sys.exit(0)

    if args.output_txt:
        txt_path = os.path.join(output_dir, "all_urls.txt")
        with open(txt_path, 'w', encoding='utf-8') as f:
            f.write("\n".join(urls))
        print(f"URL列表已保存: {txt_path}")

    filtered = [u for u in urls if any(u.lower().endswith(f".{ext}") for ext in ALLOWED_EXTS)]
    if not filtered:
        print(f"没有符合扩展名的URL（{', '.join(ALLOWED_EXTS)}）")
        shutil.rmtree(temp_dir, ignore_errors=True)
        sys.exit(0)

    print(f"开始下载 {len(filtered)} 个文件...\n")

    stats = {'success': 0, 'failed': 0, 'skipped': 0}
    lock = threading.Lock()
    pbar = tqdm(total=len(filtered), desc="下载进度", unit="个", ncols=80)

    session = requests.Session()
    session.headers.update({'User-Agent': USER_AGENT})

    with ThreadPoolExecutor(max_workers=args.threads) as executor:
        futures = [executor.submit(download_url, session, u, output_dir, pbar, stats, lock) for u in filtered]
        for f in as_completed(futures):
            f.result()

    pbar.close()
    print(f"\n下载完成: 成功{stats['success']} 失败{stats['failed']} 跳过{stats['skipped']}")
    print(f"文件保存路径: {output_dir}")

    # 清理临时目录
    shutil.rmtree(temp_dir, ignore_errors=True)
    print(f"已清理临时文件: {temp_dir}")

    try:
        os.startfile(output_dir)
    except Exception:
        pass


if __name__ == "__main__":
    main()
