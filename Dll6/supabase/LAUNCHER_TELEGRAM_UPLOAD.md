# Замена AxiomLauncher.exe в Telegram-боте

Эта инструкция предназначена для агента, который должен заменить файл,
выдаваемый командой `/download`.

Бот не загружает EXE при каждом запросе. Новый файл один раз отправляется в
Telegram от имени Axiom-бота, после чего полученный `file_id` сохраняется в
секрете Supabase `AXIOM_LAUNCHER_FILE_ID`. Команда `/download` отправляет
пользователям уже сохранённый Telegram-файл.

## Что потребуется

- готовый и проверенный `AxiomLauncher.exe`;
- токен именно того бота, в котором работает `/download`;
- числовой Telegram chat ID владельца, куда можно отправить служебную копию;
- доступ к Supabase-проекту `vljgmubfztmxsyiwrity`.

Не записывайте токен бота в репозиторий, `.env.example`, сообщения коммитов,
логи или скриншоты. `file_id` также храните как Supabase secret.

## 1. Проверить выбранный файл

Откройте PowerShell в корне репозитория и укажите абсолютный путь к EXE:

```powershell
$LauncherPath = (Resolve-Path 'Dll6\x64\Release\AxiomLauncher.exe').Path
$Launcher = Get-Item -LiteralPath $LauncherPath
$Launcher | Select-Object FullName, Length, LastWriteTime
Get-FileHash -LiteralPath $LauncherPath -Algorithm SHA256
```

Перед загрузкой зафиксируйте показанные размер и SHA-256 в отчёте агенту.
Убедитесь, что это нужная пользовательская сборка, а не промежуточный файл из
другой рабочей папки.

## 2. Получить токен и chat ID без сохранения в файлы

Токен берётся у `@BotFather` для действующего Axiom-бота. Chat ID должен быть
числовым ID владельца/администратора. Введите оба значения интерактивно:

```powershell
$BotTokenSecure = Read-Host 'Telegram bot token' -AsSecureString
$TokenPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($BotTokenSecure)
$BotToken = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($TokenPointer)
$AdminChatId = Read-Host 'Owner Telegram numeric chat ID'
```

Не выводите `$BotToken` в консоль. Загружать файл необходимо токеном того же
бота: `file_id`, полученный другим ботом, здесь работать не будет.

## 3. Один раз загрузить EXE в Telegram

Сначала владелец должен открыть диалог с ботом и нажать Start. Затем выполните:

```powershell
$ResponseJson = & curl.exe --silent --show-error --fail `
  -X POST "https://api.telegram.org/bot$BotToken/sendDocument" `
  -F "chat_id=$AdminChatId" `
  -F "document=@$LauncherPath" `
  -F "caption=Axiom Launcher candidate for /download"

$Response = $ResponseJson | ConvertFrom-Json
if (-not $Response.ok -or [string]::IsNullOrWhiteSpace($Response.result.document.file_id)) {
  throw 'Telegram did not return a document file_id.'
}
$LauncherFileId = [string]$Response.result.document.file_id
"Telegram upload succeeded; file name: $($Response.result.document.file_name)"
```

Не публикуйте полный JSON-ответ: он содержит служебные данные чата. Значение
`$LauncherFileId` не нужно добавлять в Git.

## 4. Заменить secret в Supabase

Если Supabase CLI ещё не авторизован, выполните `npx supabase login`. Затем:

```powershell
npx supabase secrets set `
  "AXIOM_LAUNCHER_FILE_ID=$LauncherFileId" `
  --project-ref vljgmubfztmxsyiwrity
```

Повторно деплоить `axiom-bot` не требуется: Edge Function читает secret во
время выполнения. Не изменяйте `TELEGRAM_BOT_TOKEN` и остальные секреты.

## 5. Проверить выдачу

1. Откройте бота как обычный пользователь.
2. Выполните `/download`.
3. Убедитесь, что бот отправил документ, а не ссылку.
4. Скачайте документ из ответа и сравните его размер и SHA-256 с исходным EXE.
5. Запустите проверку также из второго Telegram-аккаунта: файл должен
   отправляться сразу, поскольку Telegram уже хранит его по `file_id`.

Если бот прислал старый файл, проверьте активное значение имени секрета — оно
должно быть ровно `AXIOM_LAUNCHER_FILE_ID` — и повторите `/download` новым
сообщением. Старые сообщения в Telegram, разумеется, сохранят старое вложение.

## 6. Очистить секрет из памяти PowerShell

```powershell
$BotToken = $null
$BotTokenSecure = $null
$LauncherFileId = $null
if ($TokenPointer -ne [IntPtr]::Zero) {
  [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($TokenPointer)
  $TokenPointer = [IntPtr]::Zero
}
```

В итоговом отчёте укажите только:

- SHA-256 и размер загруженного EXE;
- успешность обновления Supabase secret;
- результат проверки `/download`.

Токен бота, chat ID и полный `file_id` в отчёт не включайте.
