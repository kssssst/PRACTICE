# Исправления CMakeLists.txt для успешной сборки

## Проблема
Проект не собирался из-за ошибок при генерации RPC кода MIDL компилятором.

## Решение

### 1. Единая директория для MIDL файлов
**До:** Оба проекта генерировали MIDL файлы в разные директории
- TrayApp: `${CMAKE_CURRENT_BINARY_DIR}/rpc` → `build/rpc`
- TrayService: `${CMAKE_CURRENT_BINARY_DIR}/rpc` → `build/service/rpc`

**После:** Оба проекта используют единую директорию
```cmake
set(RPC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/rpc")  # Единая директория build/rpc
```

### 2. Правильный синтаксис MIDL команды
**До:** Неправильные флаги
```bash
midl.exe /acf ... /client stub /out ... .idl  # ❌ MIDL1013 error
```

**После:** Явно указаны выходные файлы
```bash
midl.exe /acf ... 
         /cstub "${RPC_OUTPUT_DIR}/TrayService_c.c"
         /sstub "${RPC_OUTPUT_DIR}/TrayService_s.c"
         /header "${RPC_OUTPUT_DIR}/TrayService.h"
         ...idl
```

Флаги:
- `/cstub` - файл клиентского стаба (для TrayApp)
- `/sstub` - файл серверного стаба (для TrayService)
- `/header` - заголовочный файл с определением интерфейса

### 3. Устранение дублирования
**До:** `add_custom_command` был в обоих файлах
- CMakeLists.txt
- service/CMakeLists.txt

**После:** `add_custom_command` только в главном CMakeLists.txt
- MIDL запускается один раз
- Оба проекта используют сгенерированные файлы из общей директории

### 4. Структура выходных файлов

```
build/
├── rpc/
│   ├── TrayService.h          (заголовок - используется обоими)
│   ├── TrayService_c.c        (клиент - используется TrayApp)
│   └── TrayService_s.c        (сервер - используется TrayService)
├── Release/
│   └── TrayApp.exe            ✅ GUI приложение
└── service/Release/
    └── TrayService.exe        ✅ Служба Windows
```

## Результат

Теперь сборка должна успешно создать оба EXE файла:
- ✅ `build/Release/TrayApp.exe` - графическое приложение с RPC клиентом
- ✅ `build/service/Release/TrayService.exe` - Windows служба с RPC сервером

## Проверка

1. В MIDL сгенерирует:
   - ✅ `StopService()` и `GetServiceStatus()` в клиентском коде
   - ✅ `ITrayService_v1_0_s_ifspec` в серверном коде
   - ✅ `TrayServiceBinding` в обоих коде

2. Линкер найдет все необходимые символы:
   - ✅ TrayApp линкует `TrayService_c.c`
   - ✅ TrayService линкует `TrayService_s.c`
