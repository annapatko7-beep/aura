; Установщик Selfvoy для Windows (NSIS). Собирается в CI после windeployqt:
;   makensis client\packaging\windows\selfvoy.nsi
; Ставит программу в профиль пользователя (права администратора не нужны),
; создаёт ярлыки в «Пуске» и на рабочем столе, регистрирует удаление.
Unicode true
!define APP "Selfvoy"
!define DIST "..\..\..\dist"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Selfvoy"

Name "${APP}"
Icon "selfvoy.ico"
UninstallIcon "selfvoy.ico"
OutFile "..\..\..\Selfvoy-Setup.exe"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\Selfvoy"
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Selfvoy"
  SetOutPath "$INSTDIR"
  File /r "${DIST}\*.*"
  CreateDirectory "$SMPROGRAMS\Selfvoy"
  CreateShortcut "$SMPROGRAMS\Selfvoy\Selfvoy.lnk" "$INSTDIR\selfvoy-client.exe"
  CreateShortcut "$DESKTOP\Selfvoy.lnk" "$INSTDIR\selfvoy-client.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "Selfvoy"
  WriteRegStr HKCU "${UNINST}" "Publisher" "Selfvoy"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\selfvoy-client.exe"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\Selfvoy\Selfvoy.lnk"
  RMDir "$SMPROGRAMS\Selfvoy"
  Delete "$DESKTOP\Selfvoy.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${UNINST}"
SectionEnd
