#ifndef Stage
  #error Stage must be supplied by package.ps1
#endif
#ifndef Output
  #define Output "."
#endif
[Setup]
AppId={{7790FAF4-8FD9-47AF-9FF3-28DC52404648}
AppName=IX-Ray SDK — ИИ-помощник
AppVersion=0.1.0
AppPublisher=IX-Ray integration test
DefaultDirName={localappdata}\Programs\IXRay-AI-Assistant
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Output}
OutputBaseFilename=IXRay-AI-Assistant-0.1.0-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\LevelEditor.exe
CloseApplications=yes
RestartApplications=no
DisableDirPage=no
[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Icons]
Name: "{autoprograms}\IX-Ray SDK — ИИ-помощник"; Filename: "{app}\bin\LevelEditor.exe"; Parameters: "-fsltx ""{app}\fs.ltx"""; WorkingDir: "{app}"
[Run]
Filename: "{app}\bin\LevelEditor.exe"; Parameters: "-fsltx ""{app}\fs.ltx"""; WorkingDir: "{app}"; Description: "Запустить Level Editor"; Flags: nowait postinstall skipifsilent
[Code]
var
  ResourcePage: TInputDirWizardPage;
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
procedure InitializeWizard();
begin
  ResourcePage := CreateInputDirPage(wpSelectDir, 'Ресурсы проекта',
    'При необходимости укажите папку rawdata своего проекта.',
    'Пустое поле оставит отдельные тестовые ресурсы. Выбранная папка должна содержать objects, textures и levels. Файлы в ней не заменяются.', False, '');
  ResourcePage.Add('Папка rawdata (необязательно):');
end;
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and FileExists(ExpandConstant('{app}\bin\LevelEditor.exe')) then
  begin
    if not FileExists(ExpandConstant('{app}\build-info.json')) then
    begin
      MsgBox('Выберите отдельную пустую папку для этой тестовой сборки SDK. Совместимость с установленным SDK не подтверждена.', mbError, MB_OK);
      Result := False;
    end;
  end;
  if (CurPageID = ResourcePage.ID) and (ResourcePage.Values[0] <> '') and
     not DirExists(ResourcePage.Values[0] + '\objects') then
  begin
    MsgBox('В выбранной папке отсутствует подпапка objects.', mbError, MB_OK);
    Result := False;
  end;
end;
procedure CurStepChanged(CurStep: TSetupStep);
var
  Lines: TArrayOfString;
  Index: Integer;
  Destination: String;
begin
  if (CurStep = ssInstall) and FileExists(ExpandConstant('{app}\build-info.json')) then
  begin
    // Versioned installer backup contains program files only; user data is never deleted.
    Destination := ExpandConstant('{app}\_previous_build');
    if DirExists(Destination) then
      RaiseException('Папка _previous_build уже существует. Переместите её перед обновлением, чтобы сохранить возможность отката.');
    ForceDirectories(Destination);
    if DirExists(ExpandConstant('{app}\bin')) then
      if not RenameFile(ExpandConstant('{app}\bin'), Destination + '\bin') then
        RaiseException('Не удалось сохранить предыдущие бинарники. Закройте SDK перед обновлением.');
    FileCopy(ExpandConstant('{app}\build-info.json'), Destination + '\build-info.json', False);
  end;
  if (CurStep = ssPostInstall) and (ResourcePage.Values[0] <> '') then
  begin
    if LoadStringsFromFile(ExpandConstant('{app}\fs.ltx'), Lines) then
    begin
      for Index := 0 to GetArrayLength(Lines) - 1 do
        if Pos('$server_data_root$', Trim(Lines[Index])) = 1 then
          Lines[Index] := '$server_data_root$ = false| false| ' + AddBackslash(ResourcePage.Values[0]);
      SaveStringsToUTF8File(ExpandConstant('{app}\fs.ltx'), Lines, False);
    end;
  end;
end;
