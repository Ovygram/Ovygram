import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
SRC = REPO / 'Telegram' / 'SourceFiles'


class AutoUpdateClientTests(unittest.TestCase):
    def test_pinned_update_public_key_matches_key_id(self):
        header = (SRC / 'ovygram' / 'auto_update_verify.h').read_text(encoding='utf-8')
        key_id = re.search(r'kPinnedUpdateKeyId\s*=\s*"([^"]+)"', header)
        pub_b64 = re.search(r'kPinnedUpdatePublicKeyBase64\s*=\s*"([^"]+)"', header)
        self.assertIsNotNone(key_id)
        self.assertIsNotNone(pub_b64)
        raw = base64.b64decode(pub_b64.group(1), validate=True)
        self.assertEqual(len(raw), 32)
        self.assertEqual(hashlib.sha256(raw).hexdigest()[:16], key_id.group(1))
        self.assertEqual(key_id.group(1), '8e957eed7ec45741')
        # Separate from the plugin registry key (543138e5eaa5851c).
        self.assertNotEqual(key_id.group(1), '543138e5eaa5851c')

    def test_wiring_preserves_native_banner_and_protected_channel_dialog(self):
        checker = (SRC / 'core' / 'update_checker.cpp').read_text(encoding='utf-8')
        self.assertIn('Ovygram::AutoUpdate::Instance()', checker)
        self.assertIn('Ovygram::AutoUpdate::CheckReadyUpdate()', checker)

        launcher = (SRC / 'platform' / 'win' / 'launcher_win.cpp').read_text(encoding='utf-8')
        self.assertIn('Ovygram::AutoUpdate::LaunchUpdater(', launcher)

        dialogs = (SRC / 'dialogs' / 'dialogs_widget.cpp').read_text(encoding='utf-8')
        self.assertIn('u"Ovygram"_q', dialogs)

        advanced = (SRC / 'settings' / 'sections' / 'settings_advanced.cpp').read_text(encoding='utf-8')
        self.assertIn('Ovygram::ProtectSurface(box);', advanced)
        self.assertIn('Ovygram::BuildUpdateChannelBox(', advanced)
        self.assertIn('advanced/update_channel', advanced)

        cmake = (REPO / 'Telegram' / 'cmake' / 'ovygram.cmake').read_text(encoding='utf-8')
        self.assertIn('SourceFiles/ovygram/auto_update.cpp', cmake)
        self.assertIn('SourceFiles/ovygram/auto_update_verify.cpp', cmake)
        self.assertIn('SourceFiles/ovygram/auto_update_view.cpp', cmake)
        self.assertIn('SourceFiles/_other/ovygram_updater_win.cpp', cmake)
        self.assertIn('add_executable(OvygramUpdater WIN32', cmake)

        updater_win = (SRC / '_other' / 'ovygram_updater_win.cpp').read_text(encoding='utf-8')
        self.assertIn('telegramforceportable', updater_win)
        self.assertIn('TerminateLingeringPythonHosts', updater_win)
        self.assertIn('SwapPythonDirectory', updater_win)
        self.assertIn('ovygram_build.json', updater_win)

    def test_cpp_ed25519_verification_and_channel_selection_interop(self):
        qt_prefix = os.environ.get('OVYGRAM_QT6_PREFIX', '/tmp/ovygram-qt/6.11.0/gcc_64')
        qt_include = Path(qt_prefix) / 'include'
        qt_lib = Path(qt_prefix) / 'lib'
        if not (qt_include / 'QtCore' / 'QJsonObject').is_file():
            self.skipTest('Qt6Core headers not available for native C++ verification test')
        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
            from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
        except ImportError:
            self.skipTest('cryptography package not installed')

        key = Ed25519PrivateKey.generate()
        pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        kid = hashlib.sha256(pub).hexdigest()[:16]

        def sign_doc(doc: dict) -> bytes:
            payload = {**doc, 'key_id': kid}
            canonical = json.dumps(payload, sort_keys=True, separators=(',', ':'),
                                   ensure_ascii=False).encode('utf-8')
            sig = base64.b64encode(key.sign(canonical)).decode('ascii')
            return json.dumps({**payload, 'signature': sig}, ensure_ascii=False).encode('utf-8')

        manifest_bytes = sign_doc({
            'schema': 1,
            'issued_at': '2026-11-26T20:00:00Z',
            'valid_until': '2026-12-03T20:00:00Z',
            'channels_order': ['stable', 'beta', 'canary'],
            'platforms': {
                'win64': {
                    'stable': {
                        'build': 2,
                        'ovygram_version': '0.1.0',
                        'telegram_version': '7.4.6',
                        'telegram_version_num': 7004006,
                        'file_name': 'ovygram-0.1.0-7.4.6-win64-release.zip',
                        'sha256': 'a' * 64,
                        'size': 52428800,
                        'url': 'https://github.com/Ovygram/Ovygram/releases/download/t1/pkg.zip',
                        'commit': '1111aaaa',
                        'notes': 'Стабильный релиз\n"кавычки" & слэш / и таб\tOK',
                        'published_at': '2026-11-26T19:00:00Z',
                    },
                    'beta': {
                        'build': 3,
                        'ovygram_version': '0.1.1-beta.1',
                        'telegram_version': '7.4.6',
                        'telegram_version_num': 7004006,
                        'file_name': 'ovygram-0.1.1-beta.1-7.4.6-win64-release.zip',
                        'sha256': 'b' * 64,
                        'size': 53000000,
                        'url': 'https://github.com/Ovygram/Ovygram/releases/download/t2/pkg.zip',
                        'commit': '2222bbbb',
                        'notes': 'Бета с автообновлением',
                        'published_at': '2026-11-26T19:30:00Z',
                    },
                    'canary': {
                        'build': 4,
                        'ovygram_version': '0.1.1-beta.1',
                        'telegram_version': '7.4.6',
                        'telegram_version_num': 7004006,
                        'file_name': 'ovygram-0.1.1-beta.1-7.4.6-win64-release.zip',
                        'sha256': 'c' * 64,
                        'size': 53100000,
                        'url': 'https://github.com/Ovygram/Ovygram/releases/download/t3/pkg.zip',
                        'commit': '3333cccc',
                        'notes': 'Ночная сборка',
                        'published_at': '2026-11-26T19:50:00Z',
                    },
                },
            },
        })

        driver = r'''
#include "ovygram/auto_update_verify.h"
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <iostream>
#include <stdexcept>

static void Require(bool cond, const char *msg) {
    if (!cond) throw std::runtime_error(msg);
}

int main(int argc, char **argv) {
    Require(argc == 4, "usage: test <manifest.json> <key_id> <pub_b64>");
    QFile f(QString::fromUtf8(argv[1]));
    Require(f.open(QIODevice::ReadOnly), "open manifest");
    const QByteArray raw = f.readAll();
    const std::vector<Ovygram::PinnedUpdateKey> keys = {{
        QString::fromUtf8(argv[2]),
        QByteArray::fromBase64(argv[3]),
    }};
    const QUrl url(QStringLiteral("https://api.ovygram.opik.net/v1/updates/manifest"));
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-11-27T00:00:00Z"), Qt::ISODate).toUTC();
    QString err;
    auto verified = Ovygram::ParseAndVerifyManifest(raw, QStringLiteral("win64"), url, now, keys, &err);
    Require(verified.has_value(), ("verify failed: " + err.toStdString()).c_str());

    // Tampered byte must fail verification.
    QByteArray tampered = raw;
    tampered.replace("52428800", "52428801");
    Require(!Ovygram::ParseAndVerifyManifest(tampered, QStringLiteral("win64"), url, now, keys, &err).has_value(),
            "tampered manifest accepted");

    // Expired manifest must fail.
    const QDateTime expiredNow = QDateTime::fromString(QStringLiteral("2026-12-10T00:00:00Z"), Qt::ISODate).toUTC();
    Require(!Ovygram::ParseAndVerifyManifest(raw, QStringLiteral("win64"), url, expiredNow, keys, &err).has_value(),
            "expired manifest accepted");

    // Channel selection from current 0.1.0-7.4.6 build #1.
    Ovygram::InstalledVersion cur{ QStringLiteral("0.1.0"), 7004006, 1, QStringLiteral("0000") };
    auto pickStable = Ovygram::SelectUpdateForChannel(*verified, Ovygram::UpdateChannel::Stable, cur);
    Require(pickStable && pickStable->build == 2, "stable should pick build 2");
    auto pickBeta = Ovygram::SelectUpdateForChannel(*verified, Ovygram::UpdateChannel::Beta, cur);
    Require(pickBeta && pickBeta->build == 3, "beta should pick build 3");
    auto pickCanary = Ovygram::SelectUpdateForChannel(*verified, Ovygram::UpdateChannel::Canary, cur);
    Require(pickCanary && pickCanary->build == 4, "canary should pick build 4");

    // Rollback protection: build #4 must not downgrade to stable build #2.
    Ovygram::InstalledVersion newer{ QStringLiteral("0.1.1-beta.1"), 7004006, 4, QStringLiteral("3333cccc") };
    Require(!Ovygram::SelectUpdateForChannel(*verified, Ovygram::UpdateChannel::Stable, newer).has_value(),
            "downgrade should be refused");
    Require(!Ovygram::SelectUpdateForChannel(*verified, Ovygram::UpdateChannel::Canary, newer).has_value(),
            "same commit/build should not re-update");

    // URL allowlist checks.
    Require(Ovygram::IsAllowedDownloadUrl(QUrl(QStringLiteral("https://github.com/Ovygram/Ovygram/releases/download/x/y.zip")), url), "github allowed");
    Require(Ovygram::IsAllowedDownloadUrl(QUrl(QStringLiteral("https://objects.githubusercontent.com/file")), url), "gh objects allowed");
    Require(!Ovygram::IsAllowedDownloadUrl(QUrl(QStringLiteral("https://evil.example/pkg.zip")), url), "evil host rejected");
    Require(!Ovygram::IsAllowedDownloadUrl(QUrl(QStringLiteral("http://github.com/pkg.zip")), url), "plain http rejected");
    std::cout << "CPP_AUTO_UPDATE_VERIFY_OK\n";
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            manifest_path = tmp_dir / 'manifest.json'
            manifest_path.write_bytes(manifest_bytes)
            driver_path = tmp_dir / 'driver.cpp'
            driver_path.write_text(driver, encoding='utf-8')
            binary = tmp_dir / 'verify_test'
            subprocess.check_call([
                'g++', '-std=c++20', '-fPIC', '-Wall', '-Wextra', '-Werror',
                f'-I{SRC}', f'-I{qt_include}', f'-I{qt_include / "QtCore"}',
                str(SRC / 'ovygram' / 'auto_update_verify.cpp'),
                str(driver_path),
                f'-L{qt_lib}', f'-Wl,-rpath,{qt_lib}',
                '-lQt6Core', '-lcrypto',
                '-o', str(binary),
            ])
            out = subprocess.check_output(
                [str(binary), str(manifest_path), kid, base64.b64encode(pub).decode('ascii')],
                text=True,
            )
            self.assertIn('CPP_AUTO_UPDATE_VERIFY_OK', out)


if __name__ == '__main__':
    unittest.main()
