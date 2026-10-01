# M6 staging path repair — authorized bounded ownership

Author: /root/lean_refresh_review. Only tools/stage_macos_package.py and new tests/test_macos_package.py may change; root owns CMake registration and independent review.

The actual stage() preflight accepted lexical out/../escaped.app and an out parent symlink to another directory. Both private fixtures created Contents/MacOS outside the intended ignored output root before stopping at a deliberately absent binary; no deployment tools ran. Preserve the actual RED and source SHA.

Resolve the canonical workspace and allowed out root, reject a symlink at out itself, reject an existing or dangling-symlink target, resolve the full target and enforce strict descendant/out/.app membership before any mkdir/copy/tool. Keep the existing Release and explicitly empty SQLite-observer cache gates. Ordinary new/nested app directories and an absent out root remain usable. Parent aliases that resolve inside out remain usable. This is a local preflight correction; no claim of filesystem race immunity against a concurrent hostile directory replacement.

Tests invoke actual stage() in private temporary ROOT/build fixtures. Negative paths must raise ValueError before filesystem side effects/copy/tool; positive preflight stops at a test-only copy sentinel after creating only the expected target. No macdeployqt, loader edits, codesign, GUI, engine, or shared build runs. Protect Release/no-observer rejection with the same absence-of-effects assertion. Root should register the pure Python test in the existing Python3_FOUND block.
