; hellonsis.nsi — M25 installer probe: a classic 32-bit NSIS setup.exe that
; installs hello32gui.exe, creates Start Menu + Desktop shortcuts and an
; Uninstall registry entry, and writes an uninstaller. Run with /S for silent.
; Build: makensis hellonsis.nsi  ->  hellonsis-setup.exe
Unicode true
Name "Hello NSIS"
OutFile "hellonsis-setup.exe"
InstallDir "$PROGRAMFILES\Hello NSIS"
RequestExecutionLevel user
Page directory
Page instfiles
Section "Install"
  SetOutPath "$INSTDIR"
  File "hello32gui.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Hello NSIS"
  CreateShortcut "$SMPROGRAMS\Hello NSIS\Hello NSIS.lnk" "$INSTDIR\hello32gui.exe"
  CreateShortcut "$DESKTOP\Hello NSIS.lnk" "$INSTDIR\hello32gui.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HelloNSIS" "DisplayName" "Hello NSIS"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HelloNSIS" "UninstallString" "$INSTDIR\uninstall.exe"
SectionEnd
Section "Uninstall"
  Delete "$INSTDIR\hello32gui.exe"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
