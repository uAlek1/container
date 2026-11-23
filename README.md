# container

Минималистичный контейнер на C++23, реализующий базовые принципы работы: изоляцию процессов через Linux namespaces, управляющий демон, shim-процессы для восстановления доступа к контейнерам в случае падения демона и интерактивный PTY.

Проект состоит из трёх исполняемых файлов:
- `container-cli` — клиент для управления контейнерами.
- `container-daemon` — фоновый демон, хранящий состояние и управляющий жизненным циклом.
- `container-shim` — процесс-обёртка для каждого контейнера.

## Сборка

Для сборки требуются:
- CMake
- Компилятор с поддержкой C++23
- `wget` или `curl` (для скачивания тестового rootfs)

```bash
# 1. Скачать тестовый rootfs Alpine (по умолчанию в /tmp/alpine)
./scripts/fetch_alpine.sh /tmp/alpine

# 2. Собрать проект
mkdir build && cd build
cmake ..
cmake --build .
# первая сборка может быть долгой

```

Все бинарники появятся в `build/bin/`.

## Быстрый старт 
```bash
# Запустить демон в фоне
./build/bin/container-daemon &
# Запустить контейнер (в фоне)
./build/bin/container-cli run -d /tmp/alpine /bin/sh -c 'echo hello; sleep 2'
# Посмотреть список
./build/bin/container-cli list --all
# Посмотреть логи
./build/bin/container-cli logs <id>
# Запустить интерактивный shell (с PTY)
./build/bin/container-cli run /tmp/alpine /bin/sh
```

## CLI-команды
### run
Запустить контейнер.
``` bash

container-cli run [-d] [--tty] [--cwd DIR] <rootfs> [<cmd> [args...]]
-d, --detach — запустить в фоне, напечатать id и выйти.

--tty — выделить PTY даже в detached-режиме.

--cwd DIR — рабочая директория внутри контейнера (абсолютный путь).

Если <cmd> не задан, используется /bin/sh.

Всё после <cmd> передаётся команде как есть, даже если начинается с - (как в docker run). Например, run -d /tmp/alpine /bin/sh -c 'echo hi'
```
### list
Показать список контейнеров.

``` bash
container-cli list [--all]
--all — включить остановленные и завершённые контейнеры.

Без --all показывает только RUNNING и CREATED.
```

### logs
Показать логи контейнера.

```bash
container-cli logs [-f] [--tail N] <id>
-f, --follow — следить за логами в реальном времени (завершается по Ctrl+C или при завершении контейнера).

--tail N — показать последние N строк перед началом чтения.
```

### attach
Подключиться к терминалу работающего контейнера.

``` bash
container-cli attach <id>
Требует запущенного контейнера с tty=true. При подключении терминал переводится в raw mode; Ctrl+C уходит в контейнер.
```

### kill
Отправить SIGTERM контейнеру.

```bash
container-cli kill <id>
Если контейнер уже завершён, возвращает FAILED_PRECONDITION.
```

## Архитектура
``` text
CLI ──Unix socket──► Daemon ──Unix socket──► Shim ──fork/clone──► Container
                       │                      │
                       └── state.pb           └── PTY / log file
```

### Демон (container-daemon)
- Слушает Unix-сокет ($XDG_RUNTIME_DIR/container-runtime/daemon.sock).
- Принимает ClientMessage от CLI, отправляет ServerMessage.
- Управляет жизненным циклом контейнеров: Create, List, Kill, Logs, Attach.
- Запускает container-shim через fork + setsid + execv.
- При старте выполняет recovery: сканирует state.pb на диске, подключается к живым shim'ам, восстанавливает реестр.
- Фоновый StatePoller раз в секунду опрашивает shim'ы для RUNNING контейнеров, чтобы обновлять состояние (например, RUNNING → EXITED, когда процесс завершился).

### Shim (container-shim)
- Запускается демоном по одному на контейнер.
- Является прямым родителем процесса контейнера: держит master_fd PTY (или лог-файл), вызывает waitpid.
- Открывает свой Unix-сокет (<state_dir>/shim.sock), слушает Status, Signal, Attach.
- Переживает рестарт демона: делает setsid(), поэтому SIGHUP от демона его не убивает. Когда демон перезапускается, recovery подключается к живому shim'у.

### Изоляция
Процесс контейнера запускается через clone() с флагами:
- CLONE_NEWPID — свой PID namespace (процесс внутри видит себя как PID 1).
- CLONE_NEWNS — свой mount namespace (изолированные монтирования).
- CLONE_NEWUSER — свой user namespace (UID 0 внутри мапится на UID пользователя снаружи, rootless).
- CLONE_NEWUTS — свой hostname.
- CLONE_NEWIPC — своя IPC.

После clone дочерний процесс:
- Ждёт sync-сигнал от родителя (чтобы родитель успел записать uid_map/gid_map).
- Монтирует /proc и /dev (tmpfs) в rootfs.
- Настраивает stdio: PTY (если tty=true) или лог-файл (если tty=false).
- Делает chroot(rootfs) и chdir("/").
- Выполняет execvp(argv[0], argv).

Родитель (shim) в это время записывает UID/GID-маппинг в /proc/<pid>/uid_map и /proc/<pid>/gid_map (через uid_map/gid_map или newuidmap/newgidmap).

## Протокол взаимодействия
Общение между CLI, демоном и shim'ом идёт через protobuf-сообщения (ClientMessage / ServerMessage), упакованные в length-prefix кадры:

``` text
[4 байта длины little-endian][protobuf-сообщение]
```
### CLI ↔ Демон
Один Unix-сокет, один ClientMessage на запрос.
Для Logs и Attach — поток ServerMessage в ответ на один запрос.
request_id присваивается клиентом и проверяется при получении ответа (защита от рассинхронизации).

### Демон ↔ Shim
Свой сокет для каждого контейнера (<state_dir>/shim.sock).
Демон подключается к shim'у и шлёт ClientMessage (ListRequest для Status, KillRequest для Signal, AttachInput для Attach).
Shim отвечает ServerMessage.

### Recovery: state.pb + shim.sock
Recovery при старте демона:
- Сканирует $XDG_RUNTIME_DIR/container-runtime/containers/*/state.pb.
- Для каждого state.pb загружает ContainerStateFile.
- Если state != RUNNING/EXITED — проверяет kill(shim_pid, 0).
- Если shim жив — подключается к shim.sock, спрашивает Status, обновляет реестр.
- Если shim мёртв — помечает EXITED, exit_code = -1, удаляет shim.sock.

Это позволяет контейнерам переживать рестарт демона (и даже SIGKILL) — ключевая ценность shim-архитектуры.

### logs: демон читает файл напрямую
Вместо того чтобы проксировать логи через shim, демон читает лог-файл ($XDG_STATE_HOME/container-runtime/logs/<id>.log) напрямую. Это проще и работает даже после смерти shim'а. Для logs -f используется sleep(100ms) + read (позже можно заменить на inotify).

## Тесты
``` bash
# Скачать rootfs
./scripts/fetch_alpine.sh /tmp/alpine

# Запустить все интеграционные тесты
./scripts/test_containerization.sh \
    ./build/bin/container-cli ./build/bin/container-daemon /tmp/alpine

./scripts/test_daemon_protocol.sh \
    ./build/bin/container-cli ./build/bin/container-daemon /tmp/alpine

./scripts/test_recovery.sh \
    ./build/bin/container-cli ./build/bin/container-daemon /tmp/alpine
``` 
Тесты проверяют:
- Контейнеризацию: запуск команды, вывод, PID 1, exit code.

- Daemon-протокол: list, kill, logs -f, attach.

- Recovery: переживание рестарта демона (SIGTERM) и аварийного завершения (SIGKILL).