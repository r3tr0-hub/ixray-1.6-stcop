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
Source: "{#Stage}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Stage}\gamedata\*"; DestDir: "{app}\gamedata"; Flags: onlyifdoesntexist uninsneveruninstall recursesubdirs createallsubdirs; Check: ShouldInstallResources
Source: "{#Stage}\rawdata\*"; DestDir: "{app}\rawdata"; Flags: onlyifdoesntexist uninsneveruninstall recursesubdirs createallsubdirs; Check: ShouldInstallResources
Source: "{#Stage}\fs*.ltx"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#Stage}\*"; DestDir: "{app}"; Excludes: "fs*.ltx"; Flags: ignoreversion
Source: "{#Stage}\Rollback.ps1"; Flags: dontcopy
[Icons]
Name: "{autoprograms}\IX-Ray SDK — ИИ-помощник"; Filename: "{app}\bin\LevelEditor.exe"; Parameters: "-fsltx fs.ltx"; WorkingDir: "{app}"
Name: "{autoprograms}\IX-Ray SDK — откат обновления"; Filename: "{app}\Rollback.cmd"; WorkingDir: "{app}"
[Run]
Filename: "{app}\bin\LevelEditor.exe"; Parameters: "-fsltx fs.ltx"; WorkingDir: "{app}"; Description: "Запустить Level Editor"; Flags: nowait postinstall skipifsilent
[Code]
var
  ResourcePage: TInputDirWizardPage;
  UpdateInstall, UpdateStarted, InstallCompleted: Boolean;

function IsOwnBuild(const Directory: String): Boolean;
var
  Bytes: AnsiString;
  Text: String;
begin
  Result := False;
  if not LoadStringFromFile(AddBackslash(Directory) + 'build-info.json', Bytes) then Exit;
  Text := String(Bytes);
  StringChangeEx(Text, ' ', '', True);
  StringChangeEx(Text, #9, '', True);
  StringChangeEx(Text, #13, '', True);
  StringChangeEx(Text, #10, '', True);
  Result := (Pos('"product":"IXRay-AI-Assistant"', Text) > 0) and
    (Pos('"compatibilityLine":"develop"', Text) > 0);
end;

function DirectoryHasContent(const Directory: String): Boolean;
var
  Item: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Directory) + '*', Item) then
  begin
    try
      repeat
        if (Item.Name <> '.') and (Item.Name <> '..') then
        begin
          Result := True;
          Break;
        end;
      until not FindNext(Item);
    finally
      FindClose(Item);
    end;
  end;
end;

function ShouldInstallResources(): Boolean;
begin
  Result := not UpdateInstall;
end;

procedure InitializeWizard();
begin
  ResourcePage := CreateInputDirPage(wpSelectDir, 'Ресурсы проекта',
    'При необходимости укажите папку rawdata своего проекта.',
    'Пустое поле оставит тестовые ресурсы, а при обновлении сохранит прежний путь. Выбранная папка должна содержать objects, textures и levels. Файлы в ней не заменяются.', False, '');
  ResourcePage.Add('Папка rawdata (необязательно):');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Directory: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Directory := ExpandConstant('{app}');
    if DirExists(Directory) and DirectoryHasContent(Directory) and not IsOwnBuild(Directory) then
    begin
      MsgBox('Выберите отдельную пустую папку для этой тестовой сборки SDK. Здесь нет подтверждённой установки ИИ-помощника; существующие файлы не будут заменены.', mbError, MB_OK);
      Result := False;
    end;
  end;
  if (CurPageID = ResourcePage.ID) and (ResourcePage.Values[0] <> '') then
    if not DirExists(AddBackslash(ResourcePage.Values[0]) + 'objects') or
       not DirExists(AddBackslash(ResourcePage.Values[0]) + 'textures') or
       not DirExists(AddBackslash(ResourcePage.Values[0]) + 'levels') then
    begin
      MsgBox('Папка rawdata должна содержать подпапки objects, textures и levels.', mbError, MB_OK);
      Result := False;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
begin
  Result := '';
  UpdateInstall := IsOwnBuild(ExpandConstant('{app}'));
  if not UpdateInstall and DirectoryHasContent(ExpandConstant('{app}')) then
  begin
    Result := 'Для первой установки требуется отдельная пустая папка.';
    Exit;
  end;
  if UpdateInstall then
  begin
    if DirExists(ExpandConstant('{app}\_previous_build')) then
    begin
      Result := 'Папка _previous_build уже существует. Выполните откат или переместите эту резервную копию перед следующим обновлением.';
      Exit;
    end;
    ExtractTemporaryFile('Rollback.ps1');
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\Rollback.ps1') +
      '" -CheckOnly -InstallationRoot "' + ExpandConstant('{app}') + '"', '', SW_HIDE, ewWaitUntilTerminated, ExitCode) then
      Result := 'Не удалось проверить работающие процессы SDK.'
    else if ExitCode <> 0 then
      Result := 'Закройте Level Editor и служебные процессы этой установки перед обновлением. Ресурсы и данные входа сохранятся.';
  end;
end;

procedure BackupRootFile(const Name: String);
var
  Source, Destination: String;
begin
  Source := ExpandConstant('{app}\') + Name;
  Destination := ExpandConstant('{app}\_previous_build\') + Name;
  if FileExists(Source) then
    if not FileCopy(Source, Destination, False) then
      RaiseException('Не удалось сохранить файл перед обновлением: ' + Name);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Lines: TArrayOfString;
  Index: Integer;
  Destination: String;
  Item: TFindRec;
begin
  if (CurStep = ssInstall) and UpdateInstall then
  begin
    Destination := ExpandConstant('{app}\_previous_build');
    ForceDirectories(Destination + '\tools');
    BackupRootFile('build-info.json');
    BackupRootFile('SHA256SUMS.json');
    BackupRootFile('Launch-LevelEditor.cmd');
    BackupRootFile('Rollback.cmd');
    BackupRootFile('Rollback.ps1');
    if FindFirst(ExpandConstant('{app}\fs*.ltx'), Item) then
    begin
      try
        repeat
          BackupRootFile(Item.Name);
        until not FindNext(Item);
      finally
        FindClose(Item);
      end;
    end;
    UpdateStarted := True;
    if not RenameFile(ExpandConstant('{app}\bin'), Destination + '\bin') then
      RaiseException('Не удалось сохранить предыдущие бинарники. Закройте SDK перед обновлением.');
    if not RenameFile(ExpandConstant('{app}\tools\ai-assistant'), Destination + '\tools\ai-assistant') then
      RaiseException('Не удалось сохранить предыдущие служебные компоненты. Закройте процессы SDK перед обновлением.');
    if not SaveStringToFile(Destination + '\backup-info.json', '{"schema":1,"product":"IXRay-AI-Assistant","complete":true}', False) then
      RaiseException('Не удалось завершить резервное копирование.');
  end;
  if CurStep = ssPostInstall then
  begin
    if ResourcePage.Values[0] <> '' then
    begin
      if not LoadStringsFromFile(ExpandConstant('{app}\fs.ltx'), Lines) then
        RaiseException('Не удалось прочитать fs.ltx.');
      for Index := 0 to GetArrayLength(Lines) - 1 do
        if Pos('$server_data_root$', Trim(Lines[Index])) = 1 then
          Lines[Index] := '$server_data_root$ = false| false| ' + AddBackslash(ResourcePage.Values[0]);
      if not SaveStringsToUTF8File(ExpandConstant('{app}\fs.ltx'), Lines, False) then
        RaiseException('Не удалось сохранить путь ресурсов в fs.ltx.');
    end;
    InstallCompleted := True;
  end;
end;

procedure DeinitializeSetup();
var
  Previous, Current: String;
  Item: TFindRec;
begin
  if UpdateStarted and not InstallCompleted then
  begin
    Previous := ExpandConstant('{app}\_previous_build');
    if DirExists(Previous + '\bin') then
    begin
      DelTree(ExpandConstant('{app}\bin'), True, True, True);
      RenameFile(Previous + '\bin', ExpandConstant('{app}\bin'));
    end;
    if DirExists(Previous + '\tools\ai-assistant') then
    begin
      DelTree(ExpandConstant('{app}\tools\ai-assistant'), True, True, True);
      RenameFile(Previous + '\tools\ai-assistant', ExpandConstant('{app}\tools\ai-assistant'));
    end;
    if FindFirst(Previous + '\*', Item) then
    begin
      try
        repeat
          if ((Item.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) and (Item.Name <> 'backup-info.json') then
          begin
            Current := ExpandConstant('{app}\') + Item.Name;
            FileCopy(Previous + '\' + Item.Name, Current, False);
          end;
        until not FindNext(Item);
      finally
        FindClose(Item);
      end;
    end;
    MsgBox('Установка прервана. Выполнена попытка восстановить прежние bin, служебные компоненты и настройки. Резервные файлы оставлены в _previous_build для проверки.', mbInformation, MB_OK);
  end;
end;
