# KoGaMaZ

Лаунчер для KoGaMaZ со встроенным соц-сервером. Форк OpenKogama Launcher.

Скачать: **[Releases → KoGaMaZ.zip](https://github.com/absolute-spa/kogamaz/releases/latest)**

## Что умеет

- **Регистрация и вход с завода.** Аккаунты не нужны ни сайту, ни почте, ни DNS.
- **Друзья, чат, посты в ленте.**
- **Статистика игрока** с уровнями: уровень считается из `exp`, за пост даётся +10.
- **Роли соавторов карт** — `owner`, `co-owner`, `tester`, `member`. Только в лаунчере, на саму игру не влияют: строить можно всем.
- **Профиль** и публичный список хостящих.

## Установка

Распакуй `KoGaMaZ.zip` в любую папку и запусти `KoGaMaZ.exe`.

Портативно: ничего не пишет в реестр, настройки и база лежат рядом с `%LOCALAPPDATA%\KoGaMaZ`.

## Как устроен соц-сервер

Здесь нет никакого «сайта» и обязательного VPS. Лаунчер поднимает **собственный локальный сервер** на `127.0.0.1` и хранит данные в `social.json`. Поэтому регистрация, вход, чат и профиль работают сразу после первого запуска — на любой машине, без настройки.

Общий сервер (`kogamaz.duckdns.org`) — **усиление, а не требование**. Он нужен ровно для одного: чтобы друзья с разных машин нашли друг друга в поиске и списке. Лаунчер пробует его отдельно по `/api/health` и включает сам, **только если он отвечает**. Мёртвый домен или упавший VPS просто оставляют локальную базу — соц-функции не отваливаются.

Оба сервера — локальный и общий — говорят на одинаковый REST API, код маршрутов зеркальный.

## Деплой общего сервера

Нужен только для общих друзей между машинами. Файлы в [`deploy/`](deploy/):

- `kogamaz-social.service` — systemd-юнит (`node server.js`, юзер `kogamaz`, порт 3100).
- `nginx-social.conf` — блок nginx с TLS и проксированием.
- `README-ru.md` — пошаговая инструкция.

Кратко: запись DuckDNS `kogamaz → IP`, Node 22.5+, каталог `social-server/` в `/opt/kogamaz/social`, `HOST=127.0.0.1`.

Также есть [`social-server/render.yaml`](social-server/render.yaml) — blueprint для Render (бесплатный хостинг). Там БД намеренно не создаётся: на бесплатных хостингах файловая система эфемерная, SQLite стёрется при каждом редеплое. Возьми бесплатный Postgres (Neon, без карты) и вставь `DATABASE_URL`.

## Развитие

```bash
cmake -S openkogama-launcher-main -B build -G "MinGW Makefiles"
cmake --build build -j
```

Тесты (Qt 6.10, MinGW):

```bash
QT_QPA_PLATFORM=offscreen build-tests/launcher-tests
QT_QPA_PLATFORM=offscreen build-tests/localsocial-test
```

Сам сервер:

```bash
cd social-server && npm install && npm run selftest
```

Портативный архив собирается скриптом `package.ps1` в корень проекта.

## Автообновление

Лаунчер сам проверяет [релизы](https://github.com/absolute-spa/kogamaz/releases) и предлагает заменить `KoGaMaZ.zip` новой версией — скачивание, распаковка и самозамена выполняются автоматически.

Требования к релизу:

- Тег вида `vX.Y.Z` (буква `v` обязательна, в коде версия хранится без неё).
- Ассет с именем, оканчивающимся на `.zip`.
- Релиз опубликован, **не** draft и **не** prerelease — иначе `/releases/latest` его не отдаст.

Версия лаунчера хранится в одном месте: [`openkogama-launcher-main/src/appversion.h`](openkogama-launcher-main/src/appversion.h).

## Credits

Created by FeelZ Dev.
