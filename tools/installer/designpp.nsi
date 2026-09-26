Unicode True
!include "MUI2.nsh"
!include "Sections.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!define WEBVIEW2_CLIENT "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
!define APP_PATHS_KEY "Software\Microsoft\Windows\CurrentVersion\App Paths\Design++.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++"
!ifndef VERSION
!error "VERSION must be supplied by build_installer.ps1"
!endif
!ifndef PAYLOAD
!error "PAYLOAD is required"
!endif
Name "Design++ ${VERSION}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Design++"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright 2026 The Design++ Authors"
VIAddVersionKey /LANG=1033 "ProductName" "Design++"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}.0"
VIAddVersionKey /LANG=1033 "FileDescription" "Design++ per-user setup"
!define MUI_ABORTWARNING
!define MUI_ICON "${__FILEDIR__}\..\..\Design++.ico"
!define MUI_UNICON "${__FILEDIR__}\..\..\Design++.ico"
!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_WELCOMEPAGE_TEXT "Install Design++ for the current Windows user.$\r$\n$\r$\nClose all Design++ windows before continuing.$\r$\n$\r$\nSetup installs Microsoft WebView2 Runtime if it is missing; an internet connection is then required. WSL2, EDA tools and PDKs are prepared separately through Tool Check. See INSTALL.md for details."
!define MUI_COMPONENTSPAGE_TEXT_TOP "Choose the Windows shortcuts and app registration you want. The application is required."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Korean"
Var PreviousInstallDir
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Design++ requires 64-bit Windows."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Design++ requires Windows 10 or later."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext current
  ReadRegStr $PreviousInstallDir HKCU "${UNINSTALL_KEY}" "InstallLocation"
FunctionEnd
Function CheckWebView2Runtime
  StrCpy $R0 "0"
  SetRegView 32
  ReadRegStr $R1 HKLM "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${If} $R1 != ""
  ${AndIf} $R1 != "0.0.0.0"
    StrCpy $R0 "1"
  ${EndIf}
  ReadRegStr $R1 HKCU "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${If} $R1 != ""
  ${AndIf} $R1 != "0.0.0.0"
    StrCpy $R0 "1"
  ${EndIf}
  SetRegView 64
FunctionEnd
Section "Design++" Main
  SectionIn RO
  Call CheckWebView2Runtime
  ${If} $R0 != "1"
    DetailPrint "Downloading and installing Microsoft WebView2 Runtime..."
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File /oname=install_webview2.ps1 "${__FILEDIR__}\install_webview2.ps1"
    nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\install_webview2.ps1"'
    Pop $R1
    ${If} $R1 != "0"
      DetailPrint "WebView2 Runtime setup failed: $R1"
      IfSilent +2
        MessageBox MB_ICONSTOP "WebView2 Runtime setup failed ($R1).$\r$\nCheck the setup details, then run setup again."
      SetErrorLevel 1
      Abort
    ${EndIf}
    Call CheckWebView2Runtime
    ${If} $R0 != "1"
      DetailPrint "WebView2 Runtime setup completed but no runtime was detected."
      IfSilent +2
        MessageBox MB_ICONSTOP "WebView2 Runtime was not detected after setup.$\r$\nRun setup again or install the runtime from Microsoft."
      SetErrorLevel 1
      Abort
    ${EndIf}
  ${EndIf}
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r "${PAYLOAD}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Design++"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\Design++.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd
Section "Start Menu shortcuts" StartMenuShortcuts
  ReadRegDWORD $R0 HKCU "${UNINSTALL_KEY}" "StartMenuShortcutsCreated"
  StrCmp $R0 1 create_start_menu_shortcuts
  StrCmp $PreviousInstallDir "" 0 create_start_menu_shortcuts
  IfFileExists "$SMPROGRAMS\Design++\Design++.lnk" start_menu_shortcut_exists
  IfFileExists "$SMPROGRAMS\Design++\Uninstall.lnk" start_menu_shortcut_exists
create_start_menu_shortcuts:
  CreateDirectory "$SMPROGRAMS\Design++"
  ClearErrors
  CreateShortcut "$SMPROGRAMS\Design++\Design++.lnk" "$INSTDIR\Design++.exe"
  IfErrors start_menu_shortcut_done
  CreateShortcut "$SMPROGRAMS\Design++\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  IfErrors start_menu_shortcut_done
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "StartMenuShortcutsCreated" 1
  Goto start_menu_shortcut_done
start_menu_shortcut_exists:
  DetailPrint "Existing Start Menu shortcuts were left unchanged."
start_menu_shortcut_done:
SectionEnd
Section /o "Desktop shortcut" DesktopShortcut
  ReadRegDWORD $R0 HKCU "${UNINSTALL_KEY}" "DesktopShortcutCreated"
  StrCmp $R0 1 create_desktop_shortcut
  IfFileExists "$DESKTOP\Design++.lnk" desktop_shortcut_exists
create_desktop_shortcut:
  ClearErrors
  CreateShortcut "$DESKTOP\Design++.lnk" "$INSTDIR\Design++.exe"
  IfErrors desktop_shortcut_done
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "DesktopShortcutCreated" 1
  Goto desktop_shortcut_done
desktop_shortcut_exists:
  DetailPrint "An existing Desktop shortcut was left unchanged."
desktop_shortcut_done:
SectionEnd
Section /o "Register app execution path (Win+R)" AppPaths
  ReadRegStr $R0 HKCU "${APP_PATHS_KEY}" ""
  ReadRegDWORD $R1 HKCU "${UNINSTALL_KEY}" "AppPathsRegistered"
  ${If} $R0 == ""
    WriteRegStr HKCU "${APP_PATHS_KEY}" "" "$INSTDIR\Design++.exe"
    WriteRegStr HKCU "${APP_PATHS_KEY}" "Path" "$INSTDIR"
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "AppPathsRegistered" 1
  ${ElseIf} $PreviousInstallDir != ""
  ${AndIf} $R0 == "$PreviousInstallDir\Design++.exe"
    WriteRegStr HKCU "${APP_PATHS_KEY}" "" "$INSTDIR\Design++.exe"
    WriteRegStr HKCU "${APP_PATHS_KEY}" "Path" "$INSTDIR"
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "AppPathsRegistered" 1
  ${ElseIf} $R1 == 1
  ${AndIf} $R0 == "$INSTDIR\Design++.exe"
    WriteRegStr HKCU "${APP_PATHS_KEY}" "" "$INSTDIR\Design++.exe"
    WriteRegStr HKCU "${APP_PATHS_KEY}" "Path" "$INSTDIR"
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "AppPathsRegistered" 1
  ${Else}
    DetailPrint "An existing Design++.exe App Paths entry was left unchanged."
  ${EndIf}
SectionEnd
Function .onInstSuccess
  SectionGetFlags ${StartMenuShortcuts} $R0
  IntOp $R0 $R0 & ${SF_SELECTED}
  ${If} $R0 == 0
    ReadRegDWORD $R1 HKCU "${UNINSTALL_KEY}" "StartMenuShortcutsCreated"
    ${If} $R1 == 1
    ${OrIf} $PreviousInstallDir != ""
      Delete "$SMPROGRAMS\Design++\Design++.lnk"
      Delete "$SMPROGRAMS\Design++\Uninstall.lnk"
      RMDir "$SMPROGRAMS\Design++"
    ${EndIf}
    DeleteRegValue HKCU "${UNINSTALL_KEY}" "StartMenuShortcutsCreated"
  ${EndIf}
  SectionGetFlags ${DesktopShortcut} $R0
  IntOp $R0 $R0 & ${SF_SELECTED}
  ${If} $R0 == 0
    ReadRegDWORD $R1 HKCU "${UNINSTALL_KEY}" "DesktopShortcutCreated"
    ${If} $R1 == 1
      Delete "$DESKTOP\Design++.lnk"
    ${EndIf}
    DeleteRegValue HKCU "${UNINSTALL_KEY}" "DesktopShortcutCreated"
  ${EndIf}
  SectionGetFlags ${AppPaths} $R0
  IntOp $R0 $R0 & ${SF_SELECTED}
  ${If} $R0 == 0
    ReadRegDWORD $R1 HKCU "${UNINSTALL_KEY}" "AppPathsRegistered"
    ReadRegStr $R2 HKCU "${APP_PATHS_KEY}" ""
    ${If} $R1 == 1
    ${OrIf} $PreviousInstallDir != ""
      ${If} $R2 == "$INSTDIR\Design++.exe"
      ${OrIf} $R2 == "$PreviousInstallDir\Design++.exe"
        DeleteRegKey HKCU "${APP_PATHS_KEY}"
      ${EndIf}
    ${EndIf}
    DeleteRegValue HKCU "${UNINSTALL_KEY}" "AppPathsRegistered"
  ${EndIf}
FunctionEnd
Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  !include "${REMOVAL}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  ReadRegDWORD $R0 HKCU "${UNINSTALL_KEY}" "StartMenuShortcutsCreated"
  ${If} $R0 == 1
    Delete "$SMPROGRAMS\Design++\Design++.lnk"
    Delete "$SMPROGRAMS\Design++\Uninstall.lnk"
    RMDir "$SMPROGRAMS\Design++"
  ${EndIf}
  ReadRegDWORD $R0 HKCU "${UNINSTALL_KEY}" "DesktopShortcutCreated"
  StrCmp $R0 1 0 +2
    Delete "$DESKTOP\Design++.lnk"
  ReadRegDWORD $R0 HKCU "${UNINSTALL_KEY}" "AppPathsRegistered"
  ${If} $R0 == 1
    ReadRegStr $R0 HKCU "${APP_PATHS_KEY}" ""
    StrCmp $R0 "$INSTDIR\Design++.exe" 0 +2
      DeleteRegKey HKCU "${APP_PATHS_KEY}"
  ${EndIf}
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
SectionEnd
