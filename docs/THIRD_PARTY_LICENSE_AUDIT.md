# 1.0.0 third-party license audit

This inventory covers the Windows ZIP and NSIS installer. It separates code
shipped with Design++ from software that Tool Check installs or invokes in WSL.
The latter is not part of the Windows package.

| Component | Distribution | License and retained text |
|---|---|---|
| WebView2 SDK loader | ZIP and installer | SDK `LICENSE.txt` and `NOTICE.txt`, copied as `WebView2-LICENSE.txt` and `WebView2-NOTICE.txt` |
| Monaco Editor 0.56.0 | Bundled editor assets | MIT `LICENSE` and `ThirdPartyNotices.txt`, copied under `assets/editor/licenses/` |
| DOMPurify 3.4.13 | Bundled through Monaco | Apache-2.0 option of its MPL-2.0 OR Apache-2.0 choice; full Apache text copied under `assets/editor/licenses/` |
| marked 14.0.0 | Bundled through Monaco | MIT and included Markdown notices, copied from `LICENSE.md` under `assets/editor/licenses/` |
| NSIS 3 | Setup executable | NSIS `COPYING`, including zlib/libpng and the LZMA module's CPL-1.0 linking exception, copied to `licenses/NSIS-COPYING.txt` |
| esbuild 0.28.1 | Build tool only | Executable is not shipped; its MIT text is retained with editor build assets |

The WebView2 Evergreen Runtime is fetched from Microsoft when needed, rather
than embedded in the installer. OpenLane 2, ORFS, Linux EDA tools, Python
packages, and PDKs are installed or referenced separately. Their licenses are
not replaced by Design++ notices. For example, Icarus Verilog is GPL-2.0-or-later,
KLayout is GPL-2.0-or-later, and Verilator offers LGPL-3.0 or Artistic-2.0.
The ORFS script license does not cover every tool or platform used by ORFS.
Bundling a WSL image, EDA binary, or PDK in a later release requires a separate
inventory of that bundle.

Design++ itself is licensed under the MIT License in the root `LICENSE` file.
The installer presents that license for acceptance before installation. These
third-party texts remain separate from the Design++ license grant.

Primary sources: [WebView2 distribution](https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution),
[Monaco](https://github.com/microsoft/monaco-editor),
[DOMPurify](https://github.com/cure53/DOMPurify),
[marked](https://github.com/markedjs/marked),
[NSIS](https://nsis.sourceforge.io/License),
[OpenLane 2](https://github.com/efabless/openlane2),
[ORFS script license](https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts/blob/master/LICENSE_BUILD_RUN_SCRIPTS),
[Icarus Verilog](https://github.com/steveicarus/iverilog),
[KLayout](https://www.klayout.de/license.html),
[Verilator](https://github.com/verilator/verilator/blob/master/docs/guide/copyright.rst).
