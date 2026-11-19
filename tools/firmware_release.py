# -*- coding: utf-8 -*-
"""固件显示版本与成功构建后的当日序号，兼容 SDK 的 Python 2.7。"""
from __future__ import print_function
import io
import json
import os
import re
import subprocess
import sys
import time
import datetime
import hashlib
import atexit

STATE_FILE = 'version-state.json'
DISPLAY_RE = re.compile(r'[A-Za-z0-9_.+-]{1,47}\Z')


def declared_tool(product_root):
    """Product 声明的版本脚本绝对路径；没有声明返回 None。"""
    path = os.path.join(product_root, 'product', 'sources.json')
    if not os.path.isfile(path):
        return None
    with io.open(path, encoding='utf-8') as stream:
        manifest = json.load(stream)
    tool = (manifest.get('version') or {}).get('tool')
    if not tool:
        return None
    full = os.path.abspath(os.path.join(product_root, tool))
    if os.path.isabs(tool) or not full.startswith(os.path.abspath(product_root) + os.sep) or not os.path.isfile(full):
        raise ValueError('Product version tool escapes the package or is missing: ' + str(tool))
    return full


def load_state(directory):
    try:
        with io.open(os.path.join(directory, STATE_FILE), encoding='utf-8') as stream:
            state = json.load(stream)
        if isinstance(state.get('date'), type(u'')) and isinstance(state.get('sequence'), int):
            return state
    except (IOError, OSError, ValueError):
        pass
    return {}


def next_sequence(directory, date, fingerprint=None):
    """自动序号：同一天且指纹相同沿用，指纹变化加一，换天从 1 开始。返回 (序号, 是否沿用)。"""
    state = load_state(directory)
    if state.get('date') != date:
        return 1, False
    if fingerprint is not None and state.get('fingerprint') == fingerprint:
        return state['sequence'], True
    return state['sequence'] + 1, False


def commit(directory, date, sequence, fingerprint=None):
    """构建成功后记录本次序号和源码指纹。"""
    if not os.path.isdir(directory):
        os.makedirs(directory)
    with io.open(os.path.join(directory, STATE_FILE), 'w', encoding='utf-8') as stream:
        stream.write(json.dumps({'date': date, 'sequence': sequence, 'fingerprint': fingerprint or ''}) + u'\n')


def run_tool(python, tool, date, sequence):
    output = subprocess.check_output([python, tool, '--date', date, '--sequence', str(sequence), '--format', 'json'])
    versions = json.loads(output.decode('utf-8'))
    display = versions.get('display')
    if not isinstance(display, type(u'')) or not DISPLAY_RE.match(display):
        raise ValueError('Product version tool returned an invalid display version: %r' % (display,))
    return versions


def parse_sequence(value):
    """把环境变量或 defconfig 里的序号转成 int；空或 0 表示自动（返回 None）。"""
    if value is None or str(value).strip() in ('', '0'):
        return None
    try:
        number = int(str(value).strip())
    except ValueError:
        raise ValueError('version sequence must be a number from 1 to 99: %r' % (value,))
    if not 1 <= number <= 99:
        raise ValueError('version sequence must be a number from 1 to 99: %r' % (value,))
    return number


def resolve(product_root, environment, state_directory, python, today, fingerprint=None, sequence=None):
    result = {'display': environment.get('HMI_DISPLAY_VERSION', ''), 'source': 'environment',
              'date': today, 'sequence': None, 'reused': False}
    if result['display']:
        return result
    tool = declared_tool(product_root)
    if tool:
        number, reused = (sequence, False) if sequence is not None else next_sequence(state_directory, today, fingerprint)
        versions = run_tool(python, tool, today, number)
        result.update(display=versions['display'], source='product', sequence=number, reused=reused)
    else:
        result['source'] = 'fallback'
    return result


def _sibling(name):
    """按路径加载同目录的工具模块；SCons 进程里 tools/ 不在 sys.path 上。"""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name + '.py')
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location('hmi_' + name, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    except ImportError:
        import imp
        return imp.load_source('hmi_' + name, path)


def _write_if_changed(path, text):
    """内容不变时不改写，避免无谓的重新编译。"""
    if os.path.exists(path):
        with io.open(path, encoding='utf-8') as stream:
            if stream.read() == text:
                return
    with io.open(path, 'w', encoding='utf-8') as stream:
        stream.write(text)


def version_python(sdk_root, environment):
    """版本脚本需要 Python 3；默认用 SDK 自带的解释器，HMI_PYTHON 可覆盖。"""
    configured = environment.get('HMI_PYTHON')
    if configured:
        return configured
    bundled = os.path.join(sdk_root, 'tools', 'env', 'tools', 'Python38', 'python3.exe')
    return bundled if os.path.isfile(bundled) else 'python3'


def prepare(app_root, sdk_root, product, output_dir, environment, configured_sequence=None):
    """生成固件构建身份与显示版本。"""
    identity = _sibling('build_identity')
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    board = environment.get('HMI_BOARD_ID') or identity.hardware_identity(product) or 'reference-board'
    values = identity.generate(app_root, sdk_root, board, os.path.join(output_dir, 'hmi_build_identity.h'), product)
    # 指纹覆盖平台/SDK/LVGL/Product 的提交与未提交改动、SDK 配置和构建选项；它不变说明固件内容不变。
    digest = hashlib.sha256()
    for key in sorted(values):
        digest.update((key + '=' + values[key] + ';').encode('utf-8'))
    sdk_config = os.path.join(sdk_root, '.config')
    if os.path.isfile(sdk_config):
        with open(sdk_config, 'rb') as stream:
            digest.update(stream.read())
    fingerprint = digest.hexdigest()
    python = version_python(sdk_root, environment)
    today = datetime.date.today().strftime('%y%m%d')
    user_sequence = parse_sequence(environment.get('HMI_VERSION_SEQUENCE') or configured_sequence)
    version = resolve(product, environment, output_dir, python, today, fingerprint, user_sequence)
    display = version['display']
    if display and not DISPLAY_RE.match(display):
        raise ValueError('HMI_DISPLAY_VERSION must fit 47 ASCII characters')
    _write_if_changed(os.path.join(output_dir, 'hmi_display_version.h'),
                      u'#define HMI_FIRMWARE_DISPLAY_VERSION ' + json.dumps(display) + u'\n')
    release = dict(output_dir=output_dir, version=version, fingerprint=fingerprint, started=time.time() - 1)
    print('HMI product id: ' + identity.product_identity(product))
    print('HMI board id  : ' + board)
    if display:
        print('HMI version   : %s (%s)' % (display, version['source']))
    if version['sequence'] is not None:
        how = ('fixed by user' if user_sequence is not None else
               'sources unchanged since the last successful build' if version['reused'] else 'automatic')
        print('METER sequence  : V%d (%s)' % (version['sequence'], how))
    return release


def finish(release, images_dir, os_image):
    """构建产生新镜像后记录当日序号。"""
    image = os.path.join(images_dir, os_image)
    # 没有产生新镜像的 scons 调用（清理、查看配置等）不占用序号。
    if not os.path.isfile(image) or os.path.getmtime(image) < release['started']:
        return
    version = release['version']
    if version['sequence'] is not None:
        commit(release['output_dir'], version['date'], version['sequence'], release['fingerprint'])


def after_build(release, images_dir, os_image):
    from SCons.Script import GetBuildFailures
    if GetBuildFailures():
        return
    try:
        finish(release, images_dir, os_image)
    except (IOError, OSError) as error:
        sys.stderr.write('HMI version state could not be saved: %s\n' % error)
        sys.stderr.flush()
        os._exit(1)


def register_version_commit(release, images_dir, os_image):
    atexit.register(after_build, release, images_dir, os_image)
