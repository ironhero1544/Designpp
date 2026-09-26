# Third-party notices

Design++ bundles the following third-party components in its Windows release.
Their licenses are separate from the Design++ project's own MIT license in
`LICENSE`.

| Component | Bundled use | License text |
|---|---|---|
| Microsoft WebView2 SDK 1.0.4078.44 | `WebView2Loader.dll` | `WebView2-LICENSE.txt` (Microsoft's BSD-style terms) and `WebView2-NOTICE.txt` |
| Monaco Editor 0.56.0 | Editor JavaScript, CSS, fonts, worker | `assets/editor/licenses/monaco-editor-LICENSE.txt` (MIT) and `monaco-editor-ThirdPartyNotices.txt` |
| DOMPurify 3.4.13 | Included in the Monaco editor bundle | `assets/editor/licenses/dompurify-LICENSE.txt` (Apache-2.0 option of MPL-2.0 OR Apache-2.0) |
| marked 14.0.0 | Included in the Monaco editor bundle | `assets/editor/licenses/marked-LICENSE.md` (MIT and Markdown notices) |
| NSIS 3 | Windows setup executable | `licenses/NSIS-COPYING.txt` (zlib/libpng; LZMA module under CPL-1.0 with its linking exception) |

esbuild 0.28.1 is used to create the editor bundle but its executable is not
shipped. Its MIT license is retained in
`assets/editor/licenses/esbuild-LICENSE.md` for build provenance.

The WebView2 Evergreen Runtime is downloaded and installed from Microsoft only
when it is missing; it is not embedded in the Design++ package. WSL, Linux EDA
tools, OpenLane 2, ORFS, and PDKs are prepared separately and are not included
in the Windows ZIP or NSIS payload. Their own licenses apply when installed.
