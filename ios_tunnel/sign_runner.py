"""Sign a built runner with an authorized development identity and device profile."""
import argparse
import fnmatch
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--identity', required=True, help='已授权开发证书名称或 SHA1')
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--bundle-id', required=True)
    parser.add_argument('--config', type=Path, help='私密融合配置；缺省关闭隧道')
    parser.add_argument('--runtime-template', type=Path,
                        help='同版本、已验收 WDA 的 .app，用于补齐当前 Xcode 的测试运行库')
    args = parser.parse_args()
    profile = plistlib.loads(subprocess.check_output(
        ['security', 'cms', '-D', '-i', str(args.profile)], stderr=subprocess.DEVNULL))
    entitlements = profile['Entitlements']
    team = entitlements['com.apple.developer.team-identifier']
    application_id = profile['ApplicationIdentifierPrefix'][0] + '.' + args.bundle_id
    if not fnmatch.fnmatchcase(application_id, entitlements['application-identifier']):
        parser.error('描述文件不允许此 Bundle ID')
    if not args.app.is_dir() or args.app.suffix != '.app':
        parser.error('app 须为构建后的 Runner.app')
    if args.out.exists():
        parser.error('输出目录已存在；使用新目录，避免覆盖现有安装包')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.app, args.out, symlinks=True)
    info_path = args.out / 'Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    info['CFBundleIdentifier'] = args.bundle_id
    info['CFBundleDisplayName'] = 'WDA Remote'
    info_path.write_bytes(plistlib.dumps(info))
    shutil.copy2(args.profile, args.out / 'embedded.mobileprovision')
    if args.runtime_template:
        frameworks = args.out / 'Frameworks'
        frameworks.mkdir(exist_ok=True)
        for item in (args.runtime_template / 'Frameworks').iterdir():
            target = frameworks / item.name
            if target.exists():
                continue
            if item.is_dir():
                shutil.copytree(item, target, symlinks=True)
            else:
                shutil.copy2(item, target)
    bundle = args.out / 'PlugIns' / 'WebDriverAgentRunner.xctest'
    if not bundle.is_dir():
        parser.error('缺少 WebDriverAgentRunner.xctest')
    config = plistlib.loads(args.config.read_bytes()) if args.config else {'Enabled': False}
    if config.get('EnrollmentOrigin'):
        settings = args.out / 'Settings.bundle'
        settings.mkdir(exist_ok=True)
        (settings / 'Root.plist').write_bytes(plistlib.dumps({
            'PreferenceSpecifiers': [
                {'Type': 'PSGroupSpecifier', 'Title': 'WDA Remote',
                 'FooterText': '在网页控制台生成配对码，填入后启动 WDA。'},
                {'Type': 'PSTextFieldSpecifier', 'Title': '配对码', 'Key': 'WalletClickPairCode',
                 'DefaultValue': '', 'IsSecure': True, 'AutocapitalizationType': 'None',
                 'AutocorrectionType': 'No'},
            ],
        }))
    config_path = bundle / 'FBRemoteTunnel.plist'
    config_path.write_bytes(plistlib.dumps(config))
    config_path.chmod(0o600)
    sign = ['codesign', '--force', '--sign', args.identity, '--timestamp=none']
    # Sign inside out. A single --deep signing pass is insufficient for XCTest.
    nested = [p for p in args.out.rglob('*')
              if p.suffix in ('.dylib', '.framework', '.xctest') and not p.is_symlink()]
    for item in sorted(nested, key=lambda p: len(p.parts), reverse=True):
        subprocess.run(sign + [str(item)], check=True, stdout=subprocess.DEVNULL)
    app_entitlements = {'application-identifier': application_id,
        'com.apple.developer.team-identifier': team,
        'get-task-allow': bool(entitlements.get('get-task-allow')),
        'keychain-access-groups': [application_id]}
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / 'entitlements.plist'
        path.write_bytes(plistlib.dumps(app_entitlements))
        subprocess.run(sign + ['--entitlements', str(path), str(args.out)], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(args.out)], check=True)
    print('签名并校验完成：' + str(args.out))


if __name__ == '__main__':
    main()
