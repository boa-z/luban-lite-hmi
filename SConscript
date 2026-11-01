# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
"""Luban-Lite 构建入口：选择唯一 Product、组装源码；版本与打包策略在 tools/firmware_release.py。"""
from building import *
import hashlib
import imp
import io
import json
import os

Import('AIC_ROOT')
cwd = GetCurrentDir()


def tool(name):
    return imp.load_source('hmi_' + name, os.path.join(cwd, 'tools', name + '.py'))


def config_value(symbol):
    """SCons 预处理器给出的字符串宏带引号，空值是两个引号；未定义返回 None。"""
    try:
        return selection.config_string(GetConfigValue(symbol))
    except KeyError:
        return None


selection = tool('firmware_product')
release_tool = tool('firmware_release')
platform_manifest = json.load(io.open(os.path.join(cwd, 'cmake', 'sources.json'), encoding='utf-8'))

# 固件必须显式选定唯一 Product：defconfig 的 AIC_HMI_PRODUCT_ROOT，环境变量 HMI_PRODUCT_ROOT 可覆盖。
configured_product = config_value('AIC_HMI_PRODUCT_ROOT')
product_selection, selected_by = selection.firmware_selection(os.environ.get('HMI_PRODUCT_ROOT'), configured_product)
if selected_by == 'env' and configured_product:
    print('HMI product   : environment HMI_PRODUCT_ROOT overrides defconfig ' + configured_product)
product, product_sources = selection.select_firmware(cwd, product_selection)
product = str(product)
print('HMI product   : ' + product + ' (from ' + selected_by + ')')

# CAN 升级可在 defconfig 开启（AIC_HMI_CAN_UPDATE），环境变量 HMI_CAN_UPDATE=0/1 覆盖。
can_update = os.environ.get('HMI_CAN_UPDATE')
can_update = can_update == '1' if can_update else bool(GetDepend('AIC_HMI_CAN_UPDATE'))
prj_name = os.environ.get('PRJ_NAME') or Env['PRJ_NAME']
images_dir = os.path.join(AIC_ROOT, 'output', prj_name, 'images')
hmi_dir = os.path.join(AIC_ROOT, 'output', prj_name, 'hmi')
release = release_tool.prepare(cwd, AIC_ROOT, product, hmi_dir, can_update, os.environ,
                               config_value('AIC_HMI_VERSION_SEQUENCE'))
# OS 镜像名随芯片变化；固件里的升级端点与打包工具用同一个名字。
chip = os.environ.get('PRJ_CHIP') or config_value('PRJ_CHIP') or 'd13x'
os_image = chip + '_os.itb'
release_tool.register_packaging(release, images_dir, os_image)

# LVGL、MPP 解码与显示/触摸移植来自 SDK 的 lvgl-ui 包，SDK 源码不改：Product 用到的字号和原生选项、
# GE2D 绘制开关都从应用侧叠加。
Env.Append(CPPDEFINES=selection.lvgl_defines(product))
# 关掉 Kconfig AIC_HMI_DRAW_GE2D 就改用 CPU 绘制（对照排查花屏用）；SDK 的 lv_draw_ge2d.h 只在宏未定义时才默认开启。
if not GetDepend('AIC_HMI_DRAW_GE2D'):
    Env.Append(CPPDEFINES=['LV_USE_DRAW_GE2D=0'])

# SDK 的根 SConscript 用 variant_dir=output/<prj>、duplicate=0 加载整棵树。只有 Glob() 返回的节点带
# variant_dir 映射；DefineGroup 对字符串调用 File() 会在源码旁就地编译。应用目录内用变体感知的 Glob，
# 目录外的 Product 先显式 VariantDir 再取节点，避免对象落进源码或产品仓库。
external_variants = {}


def directory_tag(path):
    """目录名的稳定短标签；SDK 的 scons 跑在 Python 2 上，两种 str 都要能处理。"""
    raw = path if isinstance(path, bytes) else path.encode('utf-8')
    return os.path.basename(path) + '-' + hashlib.sha1(raw).hexdigest()[:8]


def mapped_node(variant_path, fallback):
    found = Glob(variant_path)
    return found[0] if found else fallback


def build_source(path):
    absolute = os.path.abspath(path if os.path.isabs(str(path)) else os.path.join(cwd, str(path)))
    if absolute == cwd or absolute.startswith(cwd + os.sep):
        relative = os.path.relpath(absolute, cwd).replace('\\', '/')
        return mapped_node(relative, relative)
    source_dir = os.path.dirname(absolute)
    variant = external_variants.get(source_dir)
    if variant is None:
        variant = os.path.join(AIC_ROOT, 'output', prj_name, 'external', directory_tag(source_dir))
        Env.VariantDir(variant, source_dir, duplicate=0)
        external_variants[source_dir] = variant
    external = os.path.join(variant, os.path.basename(absolute))
    return mapped_node(external, Env.File(external))


common = ['main.c', 'platform/rtthread/hmi_execution_port.c',
          'platform/rtthread/hmi_board_port.c', 'platform/rtthread/hmi_nvm_port.c',
          'platform/rtthread/hmi_rtc_port.c', 'platform/rtthread/hmi_watchdog_port.c',
          'platform/rtthread/hmi_board_settings.c', 'platform/rtthread/hmi_eeprom_i2c.c',
          'platform/rtthread/hmi_board_color.c',
          'platform/common/hmi_display_gamma.c', 'platform/common/hmi_display_ccm.c',
          'storage/hmi_file.c', 'platform/common/hmi_diag_commands.c',
          'platform/rtthread/debug/hmi_debug_console.c', 'platform/rtthread/debug/hmi_debug_log.c']
common += [p for group in ('storage', 'diagnostics', 'core', 'runtime', 'protocol_common', 'ui_common')
           for p in platform_manifest[group]]
print('HMI sources   : ' + str(len(product_sources)))

# 前导空格：SDK 的 LINKFLAGS 是字符串，直接拼接。
link_flags = ' -Wl,--wrap=lv_user_gui_init'
# SDK 的 rt_hw_interrupt_disable 里那条 csrrci 横跨 64 字节 cache line，在 D21x 上是两次崩溃的共同位置；
# 用链接器 wrap 换成从 cache line 起点开始的拷贝，不改 SDK 源码（docs/runtime/interrupt-guard.md）。
if GetDepend('AIC_HMI_IRQ_GUARD'):
    common.append('platform/rtthread/hmi_irq_guard.S')
    link_flags += ' -Wl,--wrap=rt_hw_interrupt_disable'
    if GetDepend('AIC_HMI_IRQ_STRESS'):
        common.append('platform/rtthread/hmi_irq_stress.c')
# 看门狗复位只留 Reset flag，看不出谁占着 CPU；哨兵在复位前把现场打到串口（docs/runtime/interrupt-guard.md）。
if GetDepend('AIC_HMI_LOCKUP_SENTINEL'):
    common.append('platform/rtthread/hmi_lockup_sentinel.c')

# 所有翻译单元用同一份 LVGL 配置：platform/rtthread/lvgl/lv_conf.h 排在 include 路径最前，先取 SDK 的 lv_conf.h，
# 再叠加所选 Product 的原生选项（hmi_product_lv_conf.h 转发到 Product 的头文件，没有时为空）。
product_lvgl_config = selection.lvgl_config(product)
NL = u'\n'
forward = u'/* generated by SConscript */' + NL
if product_lvgl_config:
    forward += u'#include "%s"' % product_lvgl_config + NL
forward_path = os.path.join(hmi_dir, 'hmi_product_lv_conf.h')
if not os.path.isdir(hmi_dir):
    os.makedirs(hmi_dir)
if not os.path.isfile(forward_path) or io.open(forward_path, encoding='utf-8', newline=NL).read() != forward:
    with io.open(forward_path, 'w', encoding='utf-8', newline=NL) as stream:
        stream.write(forward)
Env.Prepend(CPPPATH=[os.path.join(cwd, 'platform', 'rtthread', 'lvgl')])
# SDK 的 LVGL 线程在显示与输入设备就绪后调用 lv_user_gui_init()；用链接器 wrap 把它接到 main.c 里的
# __wrap_lv_user_gui_init，先跑 SDK 的实现再启动本应用的 UI。
group = DefineGroup('LUBAN-LITE-HMI', [build_source(p) for p in common + [str(s) for s in product_sources]],
    depend=['AIC_HMI_APP'],
    CPPPATH=[cwd, product, hmi_dir] + selection.include_dirs(product),
    CPPDEFINES=['HMI_RTTHREAD=1'],
    LINKFLAGS=link_flags)

if can_update:
    ota = os.path.join(AIC_ROOT, 'packages', 'artinchip', 'ota')
    crypto = os.path.join(AIC_ROOT, 'packages', 'third-party', 'mbedtls', 'mbedtls')
    protocol = os.path.join(cwd, 'third_party', 'iso14229', 'src')
    Env.Append(CPPDEFINES=['HMI_ENABLE_CAN_UPDATE', 'HMI_AIC_OTA', 'HMI_UPDATE_CHIP=' + chip])
    app_update = ['platform/rtthread/hmi_update_port.c', 'platform/rtthread/hmi_update_backend_aic.c',
                  'update/hmi_update.c', 'update/hmi_package.c', 'protocols/uds/hmi_uds.c',
                  'platform/rtthread/hmi_sha256_aic.c']
    upstream_update = [os.path.join(protocol, p) for p in
                       ('server.c', 'tp.c', 'util.c', 'log.c', 'tp/isotp_c.c', 'tp/isotp-c/isotp.c')]
    # SDK 原生 OTA 源码映射进 output，不在 SDK 源码旁生成对象。
    native_build = os.path.join(hmi_dir, 'sdk-ota')
    Env.VariantDir(native_build, ota, duplicate=0)
    native_update = [os.path.join(native_build, 'ota.c'), os.path.join(native_build, 'burn.c')]
    group += DefineGroup('HMI-CAN-UPDATE', [build_source(p) for p in app_update + upstream_update] + native_update,
        depend=['AIC_HMI_APP'],
        CPPPATH=[cwd, product, hmi_dir, protocol, ota, os.path.join(crypto, 'include'), os.path.join(crypto, 'library')],
        CPPDEFINES=['UDS_SYS=0', 'UDS_TP_ISOTP_C', 'UDS_LOG_LEVEL=0', 'UDS_SERVER_DEFAULT_P2_MS=1',
                    'UDS_SERVER_RECV_BUF_SIZE=1024', 'UDS_SERVER_SEND_BUF_SIZE=1024',
                    'ISO_TP_DEFAULT_ST_MIN_US=0', 'ISO_TP_DEFAULT_RESPONSE_TIMEOUT_US=1000000'])

# 额外组件（例如 Luban-Lite Display Lab 台架工具）：Kconfig AIC_HMI_EXTRA_COMPONENTS 列目录，环境变量
# HMI_EXTRA_COMPONENTS 覆盖一次构建。每个目录自带 SConscript 并返回 DefineGroup；按相对本目录的路径调用，
# 对象才会落进 output/<prj>。Framework 不认识任何组件，也不给它们提供 API；空值什么都不加。
components = os.environ.get('HMI_EXTRA_COMPONENTS')
if components is None:
    components = config_value('AIC_HMI_EXTRA_COMPONENTS') or ''
for component in components.split():
    script = os.path.join(component if os.path.isabs(component) else os.path.join(cwd, component), 'SConscript')
    if not os.path.isfile(script):
        raise ValueError('HMI component has no SConscript: ' + component)
    print('HMI component : ' + component)
    group += SConscript(os.path.relpath(script, cwd))

Return('group')
