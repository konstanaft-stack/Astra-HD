# Astra HD r30 + VR-центр Center05 — экспериментальный выпуск

> **Экспериментальный R30 / Center05.** NewGame и mirror-редактор проверены в обычной R30 через OpenXR SIM в Test3 и VR Night City: персонаж и UI видны в обоих глазах. **В обоих профилях после выхода из редактора монитор показывает мир, а изображения обоих глаз в composed-выводе остаются чёрными. Это известное неисправленное ограничение R30; причина ещё не установлена.** Полный профильный проход, физическая HMD-приёмка R30, стабильная генерация кадров и целевой FPS не подтверждены. Принятый пользователем HMD-результат относится к mirror-редактору R29 / Test3.
>
> Два согласованных runtime-архива и их SHA256 приведены ниже. [Предыдущий r29 / Center04](https://github.com/konstanaft-stack/Astra-HD/releases/tag/astra-hd-r29-experimental-20261018) не содержит NewGame-исправление R30 и остаётся с известным чёрным экраном при создании персонажа.

Два согласованных runtime-архива для Cyberpunk 2077 2.31 на Windows x64:

- [Astra-HD-r30-native-base-20261019-experimental.zip](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r30-experimental-20261019/Astra-HD-r30-native-base-20261019-experimental.zip)
- [VR-CENTR-public-win-x64-20261019-experimental05.zip](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r30-experimental-20261019/VR-CENTR-public-win-x64-20261019-experimental05.zip)
- [SHA256SUMS.txt](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r30-experimental-20261019/SHA256SUMS.txt)

Начните с [INSTALL](https://github.com/konstanaft-stack/Astra-HD/blob/main/INSTALL.md); полный [STATUS](https://github.com/konstanaft-stack/Astra-HD/blob/main/STATUS.md). Native ZIP извлекается в отдельную папку с `bundle.json`; архив центра содержит папку `VR-CENTR`. Нужен [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows); готовый центр содержит .NET runtime. Source code ZIP не заменяет runtime-архивы.

## Изменения и actual scope

- R30 поддерживает ранее нераспознанный UI-путь создания нового персонажа. Обычная R30 в Test3 и VR Night City через SIM: персонаж и UI NewGame лично просмотрены в обоих глазах. Source full23 `ff3c7f2618bbd4f6754a633998f33ce838b61272`, renderer `FDC3A66ED3FD6991108AAE9CE64FBD0E99BB72C85CAA3FB71ADDD7CA6A321376`; архив E4CA и согласованные скрипты сохранены.
- После normal registration/Apply/Custom mirror-редактор Test3 сохранял продвижение raw/UI не менее4 мин 30 с, завершился game0/host0. Это не замер FPS и не HMD-приёмка R30.
- В normal R30 NightCity SIM аватар и UI редактора видны в обоих глазах; в одном прогоне real около 20 FPS/FG 0. Отдельные контроли в Test3 и VR Night City подтвердили: после выхода из редактора монитор показывает мир, но оба composed eyes остаются чёрными. Это известное неисправленное ограничение R30; полный profile PASS не заявляется.
- Обязательный HUD-флаг из рецепта согласуется в реально используемом INI через installer/Apply/Custom/BeforeLaunch. Для распознанного R29→R30 обновления сохраняется Custom; прочие пользовательские графические/портовые параметры не задаются этим обязательным флагом.
- Center05 сохраняет public04-only профильные исправления и прежние четыре EXE/DLL. PS-правки включают shared SR/RR defaults и сортировку по addedUtc; отсутствие common штатно для native-base.

Пользователь ранее принял mirror-редактор R29/Test3 и ручную VR-камеру в физическом HMD. Небольшое перекрытие вида панелями он явно отложил. **R30 HMD ещё pending; целевой FPS, все коллекции, новый ПК и полная VR-готовность не подтверждены.** Ореолы/мерцание AER и прочие UI-артефакты не объявлены исправленными.

Native-base сохраняет выбранные NVIDIA SR/RR/G **310.9.16.0**, обязательные framework/скрипты и editor archive. **Optional OptiScaler/NR/MFG не включён. Совместная NR+FG — следующий этап, не принятая функция выпуска.** Личный common-пакет тестовой установки не менялся и не считается содержимым публичного ZIP.

На новом ПК сначала запустите чистую игру до меню и выйдите. В центре нажмите «Сохранить чистую основу», создайте и сохраните Flat-профиль, затем создайте отдельный профиль и примените R30. Возврат — переключением на captured Flat; прямой пустой flat installer fallback отключён. Native Apply не создаёт чистый снимок автоматически. Смена профиля восстанавливает сохранённую в нём версию порта, поэтому R30 применяют в нужном профиле отдельно при закрытой игре.

**Первый Native Apply без сохранённого или проверенно унаследованного режима один раз применяет recommended preset сборки, даже к существующему UserSettings.json.** Графика может измениться; последующий сохранённый Custom/«рекомендованные» сохраняется.

Управление редактором при удержании левого grip: левый стикY — высота, X — приближение; правый стикX — orbit; L3 — сброс ручных смещений. Это не системный recenter шлема.

Один обычный Switch Test3→модифицированный VR Night City занял153,8566 с (~2 мин 34 с,n=1). Обратный обычный Switch VR Night City→Test3 занял121,605 с (n=1), с проверкой сохранности32 защищённых файлов и83 исходных сохранений. Это единичные замеры данного ПК/профилей, не загрузка мира и не обещание для другого ПК. Создание профиля не измерено. Раздельные real/FG FPS mirror Test3 не сохранены; advancing XR output не доказывает80 FPS или работу генерации в редакторе.

| Архив | Байты | Файлы | SHA256 |
|---|---:|---:|---|
| Astra-HD-r30-native-base-20261019-experimental.zip | 149216876 | 360 | `CA57BDD0B6A2D9C352732CC05542E1D1A0A86A8A18CD3775CCD0F866B9D73E28` |
| VR-CENTR-public-win-x64-20261019-experimental05.zip | 113105318 | 511 | `34741743F63E3F6DA8B00E20B9E1EF0B991B93393BBBDD1E7D58DB2E5C90CCAF` |

Оба новых ZIP полностью извлечены в audit-папки с пробелами; SHA каждого файла совпал. Для текущего native ZIP реальный скопированный installer прошёл CheckOnly с отсутствующим и существующим явно указанным settings-файлом; файлы fixture не изменились. Center05 сохранил четыре прежних EXE/DLL, прошёл bootstrap21 и применимые CPU-регрессии, включая no-common и captured Flat. Это проверки целостности и контрактов установки, не новый физический HMD-тест или полный runtime-проход после выхода из редактора.

Основа и атрибуция: [CyberpunkVR Port dariulone](https://github.com/dariulone/cyberpunk-vr-port), [Tofu Express X / dabinn](https://github.com/dabinn/cyberpunk-vr-port) и открытые framework-зависимости. История, лицензии и notices сохраняются; NVIDIA runtimes не перелицензируются под MIT, условия usvfs остаются приложенными. Исходники центра — в `vr-center/` того же репозитория.
