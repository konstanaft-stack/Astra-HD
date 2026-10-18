# Готовые компоненты VR-центра

План исполнения находится в <PROJECT_ROOT>/WORK_PLAN.ru.md. Здесь только происхождение компонентов, не второй план.

| Компонент | Зафиксированный источник | Что используем | Лицензия |
|---|---|---|---|
| usvfs | https://github.com/ModOrganizer2/usvfs/tree/57f1ea5e6ad13f7435a7af184748e6c1312c5637 | Библиотека виртуальных файлов, подключение и запуск дочернего процесса без MO2. ABI надо сверить с выбранной бинарной версией. | GPL v3+ |
| Cyberpunk basic_games | https://github.com/ModOrganizer2/modorganizer-basic_games/blob/f1207d6398fbb4adda281f2bd5c563105391baaf/games/game_cyberpunk2077.py | Порядок архивов/REDmod, фильтры, кэш, служебные файлы. Адаптация отдельных функций без PyQt/MO2 GUI. | MIT |
| Vortex | https://github.com/Nexus-Mods/Vortex/tree/56668f36d29c198e3f465acbdac5b4bc650db55e | Исходящий профиль → atomic sync; deployment manifest и владение файлами. Не импортируем Electron/Redux runtime. | GPL-3.0 |
| DLSS5Manager | https://github.com/NODIX-TECH/DLSS-5-MANAGER/tree/92a80e179bb2aff28763d05bec3c50186ab94ca9 | Существующий предоставленный Avalonia-образец: стили, карточки, компактные контролы. Сохраняем уведомления авторства; обновлятор NVIDIA не переносим. | Проверить notices при переносе каждого файла |

Локальные копии и SHA скачанных файлов: <DONOR_LIBRARY>/<owner--repo>/source.json и downloaded-files.json/adapter-source-files.json. UI: <UI_SOURCE_SNAPSHOT>/UI-REUSE.txt и source.json.

Проверенные ограничения: CyberpunkVRCompanion публикует приложение/документацию, но не полный исходный код (SOURCE_AVAILABILITY.md). Его нельзя выдавать за готовую библиотеку для переиспользования. TheCuteFatCat/Cyberpunk-2077-Modmanager (266367f07787a0784dcc5a431fb303802539ed45, MIT) изучен; его учёт собственных установленных файлов не заменяет требуемую изоляцию AppData/Saved Games, поэтому новое ядро на его основе не строим.

Состояние 12.10: самостоятельный usvfs host подключён к GameLaunch; реальный дочерний процесс проверил modlist-приоритеты, .mohidden и запись в overwrite без MO2. Версия 0.5.6.1 извлечена из сохранённого Mod.Organizer-2.5.2.7z, SHA DLL E2B766F418575021B9D350F384195CE6F23173169B37222CDEF3D7FE5495F8B5. Cyberpunk-подготовка кэша/порядка перенесена с сохранением MIT notice; вызов REDmod написан, настоящим REDmod ещё не проверен. Импорт через UI/упаковка runtime и запуск реальной игры НЕ завершены.
