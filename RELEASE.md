# Astra HD r29 + VR-центр Center04 — экспериментальный выпуск

Два согласованных runtime-архива для Cyberpunk2077 2.31 на Windows x64:

- [Astra-HD-r29-native-base-20261018-experimental.zip](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r29-experimental-20261018/Astra-HD-r29-native-base-20261018-experimental.zip)
- [VR-CENTR-public-win-x64-20261018-experimental04.zip](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r29-experimental-20261018/VR-CENTR-public-win-x64-20261018-experimental04.zip)
- [SHA256SUMS.txt](https://github.com/konstanaft-stack/Astra-HD/releases/download/astra-hd-r29-experimental-20261018/SHA256SUMS.txt)

**Начните с [INSTALL](https://github.com/konstanaft-stack/Astra-HD/blob/main/INSTALL.md).** Native ZIP извлекается в отдельную папку с bundle.json; архив центра содержит папкуVR-CENTR. Нужен [PowerShell7](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows); готовый центр содержит .NET runtime. Source code ZIP не заменяет эти runtime-архивы.

В r29 включён новый native-редактор с двумя согласованными камерами. Проверены обе картинки+UI, переходы кожа→губы→предпросмотр→кожа, сохранение ручного ракурса/reference головы, свежий tracking и Reset. Обычная r29 установлена. **HMD-комфорт, полное отсутствие дрожания/мерцания и FPS ещё не приняты**; остаточные маски/UI-артефакты и AER-ореолы остаются открытыми. Полный [STATUS](https://github.com/konstanaft-stack/Astra-HD/blob/main/STATUS.md).

Native-base сохраняет выбранные NVIDIA SR/RR/G310.9.16.0, обязательные framework/скрипты и editor archive. **Optional OptiScaler/NR/MFG не включён. Совместная NR+FG — следующий этап, не функция этого выпуска.** Полные лицензии и notices приложены, включая собственные условия NVIDIA.

На новом ПК сначала запустите чистую игру до меню и выйдите. В центре нажмите «Сохранить чистую основу», создайте и сохраните Flat-профиль, затем создайте отдельный профиль и примените в нём r29. Возврат — переключением на сохранённый Flat. Прямой пустой flat installer fallback отключён; Native Apply не создаёт чистый снимок автоматически.

**Первый Native Apply один раз применяет рекомендованный preset сборки, даже к существующему UserSettings.json, если для версии ещё не сохранён режим.** Графика может измениться; последующий сохранённый выбор Custom/«рекомендованные» сохраняется.

Управление редактором при удержании левого grip: левый стикY — высота, X — приближение; правый стикX — orbit; L3 — сброс ручных смещений.

Архивы проверены полным извлечением и SHA всех файлов, actual installer CheckOnly, no-common и captured-profile CPU-контрактами. Center04 сохраняет прежние четыре EXE/DLL; изменены PowerShell и инструкции. Это не новый физический HMD/new-PC игровой тест.

| Архив | Байты | SHA256 |
|---|---:|---|
| Astra HD r29 native-base | 146920563 | `4960F126D48B34A6ED632B7DFA854CD8321C3115E16330A0927A142BA9BAE87E` |
| VR-центр Center04 | 113102454 | `3734F396053E5EC3E4C931FF2B1ED88F1D2B9D46B4E8F5544B55B83CDD8C266C` |

Основа и атрибуция: [CyberpunkVR Port dariulone](https://github.com/dariulone/cyberpunk-vr-port), [Tofu Express X / dabinn](https://github.com/dabinn/cyberpunk-vr-port) и открытые framework-зависимости. История и лицензии сохраняются; исходники центра — в `vr-center/` того же репозитория.
