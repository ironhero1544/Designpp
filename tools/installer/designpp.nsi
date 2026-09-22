Unicode True
!include "MUI2.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
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
!define MUI_WELCOMEPAGE_TEXT "Install Design++ for the current Windows user.$\r$\n$\r$\nClose all Design++ windows before continuing.$\r$\n$\r$\nRequires Microsoft Visual C++ v14 x64 Redistributable and WebView2 Runtime. WSL2, EDA tools and PDKs are prepared separately through Tool Check. See INSTALL.md for details."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Korean"
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
FunctionEnd
Section "Design++" Main
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r "${PAYLOAD}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Design++"
  CreateShortcut "$SMPROGRAMS\Design++\Design++.lnk" "$INSTDIR\Design++.exe"
  CreateShortcut "$SMPROGRAMS\Design++\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "DisplayName" "Design++"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "DisplayIcon" "$INSTDIR\Design++.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++" "NoRepair" 1
SectionEnd
Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  !include "${REMOVAL}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Design++\Design++.lnk"
  Delete "$SMPROGRAMS\Design++\Uninstall.lnk"
  RMDir "$SMPROGRAMS\Design++"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Design++"
SectionEnd
