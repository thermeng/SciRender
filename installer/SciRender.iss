; SciRender — Inno Setup script (no file association)
; Produces SciRender-Setup.exe from windeployqt-staged package/ folder.
; Usage (CI): iscc installer/SciRender.iss /DAppVersion=0.1.0

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppName=SciRender
AppVersion={#AppVersion}
AppPublisher=SciRender
AppPublisherURL=https://github.com/thermeng/SciRender
DefaultDirName={autopf}\SciRender
DefaultGroupName=SciRender
OutputDir=../dist
OutputBaseFilename=SciRender-Setup
WizardStyle=modern
UninstallDisplayIcon={app}\SciRender.exe
DisableProgramGroupPage=yes
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "..\package\SciRender.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\package\shaders\*"; DestDir: "{app}\shaders"; Flags: recursesubdirs
Source: "..\package\*.dll"; DestDir: "{app}"; Flags: skipifsourcedoesntexist
Source: "..\package\platforms\*"; DestDir: "{app}\platforms"; Flags: recursesubdirs skipifsourcedoesntexist
Source: "..\package\styles\*"; DestDir: "{app}\styles"; Flags: recursesubdirs skipifsourcedoesntexist
Source: "..\package\translations\*"; DestDir: "{app}\translations"; Flags: recursesubdirs skipifsourcedoesntexist
Source: "..\package\README.md"; DestDir: "{app}"; Flags: skipifsourcedoesntexist

[Icons]
Name: "{autoprograms}\SciRender"; Filename: "{app}\SciRender.exe"
Name: "{autodesktop}\SciRender"; Filename: "{app}\SciRender.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; Flags: unchecked

[Run]
Filename: "{app}\SciRender.exe"; Description: "Launch SciRender"; Flags: nowait postinstall skipifsilent
