# KoGaMaZ — деплой сервера

Два варианта хостинга. Файлы лежат рядом: `server.radmin.json`, `server.vps.json`, `Caddyfile`, `kogamaz.service`.

## Общее

Сервер берётся из `%LocalAppData%\OpenKogamaLauncher\current\server` (или из KoGaMaZ после сборки — та же папка):
- скопируй папку `server` в отдельное место, например `C:\OpenKogamaServer\app` (Windows) или `/opt/kogamaz/app` (Linux, нужен .NET 10 SDK: `dotnet publish openkogama-server.csproj -c Release -o app`)
- создай папку данных рядом: `C:\OpenKogamaServer\data` или `/opt/kogamaz/data`
- положи в неё `server.json` (один из вариантов ниже)
- свои миры: закрой лаунчер, скопируй `server.db` из `%LocalAppData%\OpenKogama\OpenKogama Launcher\server\server.db` в папку данных
- запуск: `cd data && ..\app\openkogama-server.exe` (Windows) или `cd /opt/kogamaz/data && dotnet /opt/kogamaz/app/openkogama-server.dll` (Linux)

Порты по умолчанию: HTTP 8080, UDP 5055 (игра), 843-845 (web player 2012-2014), 8081 (proxy).
**Важно:** пока работает standalone-сервер, не запускай миры в самом лаунчере на этом же ПК — порты те же.

## Вариант A: Radmin VPN (с друзьями, без VPS)

1. Все ставят Radmin VPN, ты создаёшь сеть (имя + пароль), друзья входят.
2. Узнай свой Radmin IP (например `26.123.45.67`) — он в окне Radmin.
3. Возьми `server.radmin.json`, замени `RADMIN_IP` на этот IP, положи как `server.json` в папку данных.
4. Разреши в Windows Firewall порты: TCP 8080, TCP 843-845, UDP 5055 + TCP 5055.
5. Запусти сервер.
6. Друзья: лаунчер → Servers → Add server → `http://<твой-radmin-ip>:8080`.

Ограничение: сборки Unity 2022+ (2.x, 3.x) по документации отказываются от plain HTTP кроме localhost.
Старые версии (1.x, 2012) — работают. Для 3.5.x через интернет — вариант B.

## Вариант B: VPS + HTTPS (полный, для любых версий)

Нужны: VPS (1 ядро, 1 ГБ RAM хватит), домен или `<ip-with-dashes>.sslip.io` (например `203-0-113-7.sslip.io`).

1. Порты в файрволе VPS: TCP 80, 443 (Caddy), UDP 5055 (игра), TCP 843 (web player). **8080 наружу не открывать!**
2. Скопируй `app` на VPS: `/opt/kogamaz/app`, данные: `/opt/kogamaz/data`.
3. `server.json` = `server.vps.json` (замени домен; **без** `listenHost` — сервер слушает только localhost, наружу только Caddy).
4. Пользователь: `useradd --system kogamaz`, `chown -R kogamaz /opt/kogamaz/data`.
5. Ёнит `kogamaz.service` → `/etc/systemd/system/kogamaz.service` → `systemctl enable --now kogamaz`.
6. Caddy: поставь, положи `Caddyfile` (замени домен) → `systemctl reload caddy`.
7. Добавь сервер в лаунчере: `https://<домен>`, либо `"public": true` в server.json — появится в общем списке.

## Общий соц-сервер (обязательно для друзей)

Это отдельный Node.js-сервер из папки `social-server`, а не игровой сервер.
**Без него каждый запускает свой локальный сервер со своей базой**, из-за чего
друзья не находят друг друга по нику: поиск идёт по своей базе, в которой друга
просто нет. Чат, заявки в друзья и посты сломаны той же причиной.

Порт **3100** (не 8080 - там игровой сервер). Нужен **Node.js 22.5+**: сервер
собран на встроенном `node:sqlite`, в старых версиях его нет.

1. Папку `social-server` на VPS: `/opt/kogamaz/social`.
2. Проверь запуск руками: `cd /opt/kogamaz/social && PORT=3100 node server.js`
   - в логе `[kogamaz-social] listening on 127.0.0.1:3100` и `backend=...`
3. Юнит: `deploy/kogamaz-social.service` → `/etc/systemd/system/` →
   `systemctl daemon-reload && systemctl enable --now kogamaz-social`.
4. Nginx: `deploy/nginx-social.conf` → внутрь того же `server { }`, что и
   игровой сервер → `nginx -t && systemctl reload nginx`.
5. Проверь наружу: `curl https://<домен>/social/api/health` должен отдать JSON.
6. Порт 3100 наружу в файрволе **не открывать** - наружу ходит только nginx.

Данные лежат рядом с `server.js` (файл базы). Бэкап = копия этого файла.

## Photon App ID

`41f826f7-0770-4825-9d91-decd52d9460f` — сохранён в KoGaMaZ (`settings.ini` → `[photon] appId=...`, дефолт в `src/settings.cpp`).
Текущий стек OpenKogama его не использует (сервер сам эмулирует Photon на UDP 5055) — он нужен для будущих мультиплеер-функций KoGaMaZ через Photon Cloud.

## Обновление сервера

После обновления лаунчера перекопируй `%LocalAppData%\OpenKogamaLauncher\current\server` в папку `app` заново.
