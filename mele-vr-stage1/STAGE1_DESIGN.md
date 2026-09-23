# Этап 1: управление оружием с VR-контроллеров

Мод: `jrush64/MassEffect1-VR` (6DOF VR для Mass Effect LE1, dxgi.dll прокси, UE3 build 2.0.0.48602).
Статус: дизайн-документ + черновик кода (2026-09-23). Лицензия проекта: GPL-3.0.

---

## 1. Цель и не-цели

**Цель Этапа 1** — играть на контроллерах так же, как сейчас на геймпаде:

- правый контроллер = **прицел** (луч из контроллера управляет `ControlRotation` — та же запись, что у head-aim);
- триггер = выстрел, grip = ADS/плечо, стики и кнопки = движение/способности/перезарядка/смена оружия — всё через **«виртуальный XInput-геймпад»**, который игра читает как подключённый геймпад (между игрой и хуком уже стоят `XInputGetState(Ex)`);
- анимации оружия (выстрел, перезарядка, ADS) **не трогаем** — движок гоняет их от input state, и они пойдут сами;
- взгляд остаётся за головой (комфортная VR-модель: «голова держит картинку, контроллер целит»).

**Не-цели (Этапы 2–3, не в этом документе):**
- привязка модели оружия к руке, 6DOF-поз оружия, «лазерная пушка»,
- телепорт/локомоция (движение = стик, как сейчас),
- левая рука как второй «оружейный» слот (в ME1 левая рука не держит оружие — способности стреляются из тела).

## 2. На чём строим (то, что уже доказано в коде)

| Механизм | Где | Что даёт Этапу 1 |
|---|---|---|
| OpenXR-сессия на render-потоке | `xr_session.cpp::OnPresent` → фрейм-луп, `LocateHead(displayTime)` → `g_subViews` | Место для синхронизации ввода (тот же поток, то же displayTime, без лочек) |
| Событийная конвенция головы | `HeadEulerDegrees()` (`xr_session.cpp:1955`): forward = R\*(0,0,−1), yaw = atan2(fx,−fz), pitch = asin(fy) | Ту же математику применяем к ориентации правого контроллера |
| Запись прицела | `HeadAim::DriveAimWithHead(yaw,pitch)` → `APlayerController.ControlRotation` (SEH, `EnsureController`/`ControllerStable`, карантин 90 кадров) | **Новых записей в игру не появляется** — меняется только источник углов |
| Детектор «оружие выпущено» | `HeadAim::ReadWeaponModeSEH()` (1 = Combat/TightAim/…) + storm-latch + `gameMode` (1 = Мако, 7 = GUI) | Все существующие гейты (storm, Мако, меню) работают без изменений |
| Хук XInput | `vr_menu.cpp::HookedXInputGetState` / `...Ex` (~строка 180/207): сейчас passthrough реального геймпада + decouple осей + `RotateMoveStickByHeadLook` | Пункт вставки синтеза; переиспользуем те же корректировки для синтетического стейта |
| Конфиг | `Config::VrConfig` (`vr_config.h`), load/save по именам ключей в ini, `kDefaults` в меню | Добавляем 7 ключей, все по умолчанию OFF |
| Меню | ImGui-вкладки в `vr_menu.cpp` (Tracking ~944), паттерн `ResetBtn` | Новая секция «Controller Input» |
| Лог | `MELEVR::Logger::LogLine` → `MELEVR_Log.txt` рядом с игрой | Диагностика ABI-пробы и маппинга |

## 3. Карта изменений по файлам

| Файл | Изменение | Масштаб |
|---|---|---|
| `src/xr_input.h` / `src/xr_input.cpp` | **Новый модуль `MELEVR::XrInput`**: action set, actions, action spaces, per-frame sync/locate/update, aim source, синтез виртуального геймпада | ~450 строк (черновик приложен) |
| `src/xr_types.h` | +25–30 строк: `XrPath/XrActionSet/XrAction`, 12 констант, 10 структур Action API, 10 PFN (см. `patches/01`) | только добавление |
| `src/xr_session.cpp` | (a) `XrInput::Init(...)` после создания session; (b) `XrInput::OnFrame(...)` в фрейм-лупе перед weapon-state switch; (c) swap источника aim в 2 точках (on-foot ~4010, Мако ~3895); (d) render-side head-look при controller aim; (e) `XrInput::SessionInvalidated()` в `PollEvents()` при STOPPING (action spaces — session-bound) | 5 in-situ вставок, ~25 строк |
| `src/vr_menu.cpp` | (a) `HookedXInputGetState(Ex)`: ветка синтеза + `return ERROR_SUCCESS`; (b) секция «Controller Input» в вкладке Tracking | ~40 строк |
| `src/vr_config.h` / `src/vr_config.cpp` | 7 новых полей + load/save | ~25 строк |
| `src/MELEVRClean.vcxproj` | `<ClCompile Include="xr_input.cpp"/>`, `<ClInclude Include="xr_input.h"/>` | 2 строки |
| `docs/STAGE1_CONTROLLER_DESIGN.md` | этот документ | — |

**Не меняется:** `head_aim.cpp` (пишем через существующий `DriveAimWithHead`), `d3d_capture.cpp`, `le1_game.h`, рендер-путь.

## 4. Модуль `MELEVR::XrInput` (дизайн)

### 4.1 Actions и bindings

Один action set `melevr-input`, действия = полное «gamepad»-зеркало двух рук (стандартный KHR simple-controller профиль поддерживается SteamVR, Quest Link, Virtual Desktop):

| Действие | Тип | Путь биндинга |
|---|---|---|
| `right/pose`, `left/pose` | POSE | `/user/input/{r,l}/pose` |
| `right/trigger`, `right/squeeze` | FLOAT | `/user/input/right/gamepad/{trigger,squeeze}` |
| `right/grip` | BOOLEAN | `/user/input/right/gamepad/grip` |
| `right/thumbstick` | VECTOR2F | `/user/input/right/gamepad/thumbstick` |
| `right/thumbstick/click` | BOOLEAN | `/user/input/input/right/gamepad/thumbstick/click` |
| `right/{a,b,x,y}` | BOOLEAN | `/user/input/right/{a,b,x,y}` |
| `right/dpad/{up,down,left,right}` | BOOLEAN | `/user/input/right/dpad/*` |
| `left/…` (аналогично) | … | `/user/input/left/…` |

Профиль: `xrSuggestInteractionProfileBindings` c путём `/interaction_profiles/khr/simple_controller` — единственный нужный профиль; остальные runtime-специфичные профили не нужны.

### 4.2 Tracking контроллеров — через action spaces (важное решение)

Поза руки **не через** `XrReferenceSpaceCreateInfo(targetLocation=INPUT)`, а через **pose-action + `XrActionSpace`**:

```
xrCreateActionSpace(session, {action=right/pose, subactionPath=INVALID, pose=identity}, &rightSpace)
xrLocateSpace(rightSpace, g_appSpace, displayTime, &loc)   // -> XrSpaceLocation{locationFlags, pose}
```

Почему: в моде `XrReferenceSpaceCreateInfo` — **4-полевая 0.9-эра** (`type, next, referenceSpaceType, poseInReferenceSpace`, без `targetLocation/userPath`), и бандл `openxr_loader.dll` подтверждён работой именно с ней. Input-space через 4-полевой struct не создашь; а `XrActionSpaceCreateInfo` и `xrLocateSpace` — простые структуры, не зависящие от этой дилеммы. Бонус: позы рук приходят в **том же app-space, что и поза головы** (`g_subViews`) — aim и head-look живут в одной системе координат.

### 4.3 Frame loop (render thread)

```
OnFrame(appSpace, displayTime, headQuat):
  1. если session != g_attachedSession:
       xrAttachSessionActionSets(session, {g_actionSet})
       пересоздать rightSpace/leftSpace (action spaces — session-bound)
       g_attachedSession = session
  2. xrSyncInputs(instance, &n, actions[29])
  3. xrLocateSpace ×2  (правая, левая)  -> poseValid по locationFlags
  4. xrUpdateActionState ×N  -> raw-массивы val/bool/vec2
  5. smoothing aim-quaternion (slerp low-pass + head-blend) -> aimYaw/aimPitch
  6. опубликовать g_frame (InputFrame)  // единственный поток — без синхронизации
```

Ошибки любого звена → `g_frame` невалиден → **автоматический fallback на текущее поведение** (реальный геймпад passthrough + head aim). Функция не умеет «поломаться наполовину».

### 4.4 API модуля

```cpp
namespace MELEVR::XrInput {
bool Init(XrInstance, XrSession, PFN_xrGetInstanceProcAddr);  // один раз, render thread
void Shutdown();
bool IsReady() noexcept;
void OnFrame(XrSpace appSpace, XrTime displayTime, const XrQuaternionf& headQuat) noexcept;
bool GetAimDeg(float* yawDeg, float* pitchDeg) noexcept;      // сглаженный луч правой руки
bool AimActive() noexcept;                                    // true, пока aim реально от контроллера
bool BuildVirtualGamepad(XINPUT_STATE* state, const Config::VrConfig& cfg) noexcept;
}
```

## 5. Виртуальный геймпад (синтез XInput в хуке)

**Маппинг по умолчанию** (1:1 «контроллер как геймпад»; детали к §11 — верификация):

| VR-контроллер | XInput | Примечание |
|---|---|---|
| right trigger | `bRightTrigger` | **выстрел**; бинарный: > deadzone → 255, иначе 0 (дeterministic, без джиттера автоогня) |
| left trigger | `bLeftTrigger` | |
| right grip | `RIGHT_SHOULDER` (R1) | |
| left grip | `LEFT_SHOULDER` (L1) | |
| right thumbstick click | `RIGHT_THUMB` (R3) | |
| left thumbstick click | `LEFT_THUMB` (L3) | |
| a/b/x/y | A/B/X/Y | способности/перезарядка/смена оружия — по XInput-бндам ME1 (верить §11) |
| dpad | D-PAD | |
| left thumbstick | `sThumbLX/LY` | **движение**; deadzone 0.15 + rescale; проходит через тот же `RotateMoveStickByHeadLook` (бежать туда, куда смотришь) |
| right thumbstick | по умолчанию `sThumbRX/RY = 0` | голова владеет look; `controllerRightStickLook=1` → зеркалить |

Детали реализации:
- синтез происходит в `HookedXInputGetState` **и** `HookedXInputGetStateEx` (игра может опрачивать оба);
- при синтезе хук возвращает `ERROR_SUCCESS` — игра видит «подключённый» геймпад, даже если физического нет;
- `dwPacketNumber++` на каждом кадре синтеза, `dwFlags = XINPUT_FLAG_GAMEPAD`;
- меню открыто (`g_open`) → нейтральный геймпад (не меняется), ImGui читает реальный геймпад напрямую (не меняется);
- контроллеров нет/отпали → ветка passthrough реального геймпада (не меняется, байт-в-байт);
- **несмотря на синтез, состояние проходит через те же корректировки**, что и реальный: `decoupledPitch/decoupledYaw` zeroing + `moveFollowsHead` — переиспользуем, не дублируем.

**Честное ограничение:** какая именно XInput-кнопка в ME1 = «способность/перезарядка/колесо оружия» — в кодовой базе не зафиксировано (мод никогда не синтезировал кнопки, только корректировал стики). По умолчанию кладём 1:1 (A/B/X/Y), а маппинг **верифицируем** инструментом из §11 (`controllerLogRealPad`) и при необходимости в Этапе 1.1 делаем переназначаемую таблицу (ключи ini).

## 6. Источник прицела (aim source)

```
AimQuat_raw = slerp(headQuat, rightHandQuat, cfg.controllerAimHeadBlend)   // 0 = чистый контроллер
AimQuat     = slerp(AimQuat_prev, AimQuat_raw, 1 - cfg.controllerAimSmoothing)  // low-pass
yaw/pitch   = HeadEulerDegrees-конвенция (forward = R*(0,0,-1))
запись      = DriveAimWithHead(yaw, pitch)   // БЕЗ ИЗМЕНЕНИЙ — тот же санкционированный путь
```

Где переключается (2 точки, оба существующих гейта — `combatHeadAim`, weapon-out, !storm, gameMode, ctrlLive/ctrlStable — сохраняются):

1. **On-foot** (`xr_session.cpp`, блок `aimGatesPass`, ~строка 4010): вместо `yawDeg/pitchDeg` из головы — `XrInput::GetAimDeg(...)` при `cfg.controllerAim`.
   - **View-модель**: камера (ControlRotation) следует за лучом контроллера, а render-side head-look продолжает поворачивать картинку за головой (как в explore). Итог: «картинка стабильна в голове, прицел — в контроллере». В on-foot ветке это значит, что при controller aim мы НЕ делаем `SetHeadLook(0,0,false)` («aim owns the rotation»), а держим head-look (черновик помечает точку; при интеграции поднять expression из explore-ветки в хелпер — см. патч (d)).
2. **Мако** (`gameMode == 1`, ~строка 3895): тот же swap — луч контроллера управляет ствольным узлом через существующий ControlRotation-путь (не через bony-эксперименты).

`invertAimYaw/Pitch` применяются к источнику одинаково (знак входа), как сейчас.
Storm (рывок) — **не меняется** (free head-look + поворот стика): прицелить во время рывка всё равно нельзя, инверсия у движения там решена отдельным патом.

## 7. Новые настройки (все по умолчанию OFF)

| Ключ ini | Тип | Умолчание | Смысл |
|---|---|---|---|
| `controllerInput` | bool | 0 | виртуальный геймпад из контроллеров |
| `controllerAim` | bool | 0 | луч правого контроллера = прицел |
| `controllerAimSmoothing` | float 0..1 | 0.35 | low-pass на aim-кватернионе (0 = без) |
| `controllerAimHeadBlend` | float 0..1 | 0.0 | 0 = чистый контроллер, 1 = чистая голова |
| `controllerTriggerDeadzone` | float 0..0.5 | 0.15 | |
| `controllerRightStickLook` | bool | 0 | 1 = правый стик управляет look (голова перестаёт владать осями) |
| `controllerLogRealPad` | bool | 0 | лог реального XInput 2 Гц (верификация маппинга) |

## 8. Меню

Вкладка **Tracking**, новая секция `CollapsingHeader("Controller Input (Stage 1)")` после «Head Aim»: чекбоксы/слайдеры на §7 + `ResetBtn` на каждый + пояснительные `TextDisabled`-строки (маппинг по умолчанию, ссылка на §11). Профили (Profiles tab) подхватывают ключи автоматически.

## 9. ABI: верифицировано и требует проверки

Проект **не линкует OpenXR-SDK**: все типы hand-written в `xr_types.h` (сравнение по байт-лейауту), функции резолвятся из `openxr_loader.dll` через `xrGetInstanceProcAddr`. Черновик резолвит функции сам (через переданный `getProc`) — в `struct Functions` ничего не добавляем.

**Константы, зафиксированные из Khronos-хедера (cross-check: main-ветка + tag release-1.0.34, значения совпадают):**

```
XR_TYPE_ACTION_SET_CREATE_INFO            = 28
XR_TYPE_ACTION_CREATE_INFO                = 29
XR_TYPE_ACTION_SPACE_CREATE_INFO          = 38
XR_TYPE_SPACE_LOCATION                    = 42
XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51
XR_TYPE_ACTION_STATE_BOOLEAN              = 23
XR_TYPE_ACTION_STATE_FLOAT                = 24
XR_TYPE_ACTION_STATE_VECTOR2F             = 25
XR_TYPE_ACTION_STATE_POSE                 = 27
XR_TYPE_ACTION_STATE_GET_INFO             = 58
XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO   = 60
XR_TYPE_ACTIONS_SYNC_INFO                 = 61
XR_SPACE_LOCATION_ORIENTATION_VALID_BIT   = 0x1
XR_SPACE_LOCATION_POSITION_VALID_BIT      = 0x2
XrActionType: BOOLEAN_INPUT=1, FLOAT_INPUT=2, VECTOR2F_INPUT=3, POSE_INPUT=4
```

**Точки неопределённости (бандл-лоадер может быть 0.9-эры — его `XrReferenceSpaceCreateInfo` 4-полевой, что указывает на pre-final-1.0 ABI):**

1. Имена функций: `xrAttachSessionActionSets` (1.0) vs `xrSessionAttachActionSets` (0.9); `xrSuggestInteractionProfileBindings` (1.0) vs `xrCreateActionSetBindings` (0.9).
2. `XrActionCreateInfo`: в 1.0 `actionSet` передаётся **аргументом** `xrCreateAction(actionSet, ...)`, в 0.9 — полем структуры.
3. `XrActionType`: 1.0 = {1,2,3,4}, 0.9 = {0,1,2,3}.

**Решение: ABI-проба.** `XrInput::Init()` резолвит оба варианта имён, логгирует в `MELEVR_Log.txt` (`[XRINPUT] loader gen: 1.0|0.9, attach=ok, suggest=ok, sync=ok, locateSpace=ok`), и черновик компилируется под 1.0-набор. Если проба покажет 0.9 — включаем `#define MELEVR_XRINPUT_09` (фолбэк-таблица в Приложении А). Первый запуск с пробой = решение вопроса, после чего код пинимся.

## 10. Модель безопасности

- **Новых записей в память игры нет.** Aim-запись = существующий путь со всеми гардами (EnsureController, ControllerStable, карантин 90 кадров, SEH).
- Ввод — только через существующий XInput-хук; физический геймпад продолжает работать при отключённых/отвалившихся контроллерах.
- Все фичи OFF по умолчанию; любой сбой OpenXR → поведение идентично текущему (fail-safe по конструкции, не по обещанию).
- OpenXR-вызовы не пишут в память игры → SEH на них не нужен; все результаты проверены на XrSucceeded.

## 11. Тест-план (на машине разработчика; в песочнице Windows-тулчейна нет)

| # | Шаг | Критерий |
|---|---|---|
| T0 | Сборка (`scripts\build.bat`), запуск без шлема (`MELEVR_DISABLE_OPENXR`) | Меню показывает секцию, игра как прежде, в логе `[XRINPUT] disabled (no runtime)` |
| T1 | Шлем + SteamVR, все новые настройки OFF | Логи: `[XRINPUT] loader gen=…`, `ready`; поведение байт-в-байт как upstream (A/B-прогон) |
| T2 | `controllerAim=1` (остальное OFF) | Прицел следует за лучом правой руки; взгляд — за головой; Мако: ствольной узел за лучом; storm без регресса |
| T3 | `controllerInput=1` | Выстрел триггером, ADS grip'ом (или R3 — см. T4), движение левым стиком «туда, куда смотришь», способности по A/B/X/Y |
| T4 | **Верификация маппинга**: `controllerLogRealPad=1` + 10 минут игры на реальном геймпаде; сверить кнопки лога с фактическими действиями (выстрел/ADS/способности/перезарядка/колесо) и зафиксировать в документе | Таблица «кнопка XInput → действие ME1» |
| T5 | Отвал/переподключение контроллера, переход в Мако, меню, катсцены | Нет зависаний, корректный fallback, меню не «видит» контроллер |
| T6 | Тьюнинг: smoothing 0→0.6, headBlend 0→0.5 | Субъективно: прицел без джиттера, не «отстаёт» |

## 12. Вехи

| Веха | Содержание | Оценка |
|---|---|---|
| **M0** | Типы в `xr_types.h` + `XrInput::Init` с ABI-пробой + 7 ключей конфига + скелет меню (всё OFF). Первый запуск → лог пробует поколение лоадера | 1–2 дня |
| **M1** | Tracking рук + **aim source** (T2) — основная ценность Этапа | 2–3 дня |
| **M2** | Виртуальный геймпад (T3) | 1–2 дня |
| **M3** | Мако + logRealPad-верификация + тьюнинг (T4–T6) | 1 день |

## 13. Риски

| Риск | Митигация |
|---|---|
| ABI бандл-лоадера (0.9 vs 1.0) | М0-проба + Приложение А; худший случай — 0.9-фолбэк (те же структуры, другие имена/значения) |
| Маппинг кнопок ME1 неизвестен 100% | `controllerLogRealPad` (T4); при необходимости — переназначаемая таблица в Этапе 1.1 |
| Комфорт: джиттер/лаг луча | low-pass + head-blend (настройки), тьюнинг T6 |
| Ложные ожидания «как HL2» | Документировано: это «HL2-lite» — механика 100%, визуал рук/привязки оружия — Этап 2–3 |
| Perf | +~15 лёгких OpenXR-вызовов/кадр на render-потоке — пренебрежимо |

---

## Приложение A. Фолбэк 0.9-поколения (если ABI-проба покажет 0.9)

```
Имена функций:  xrSessionAttachActionSets (вместо xrAttachSessionActionSets)
                xrCreateActionSetBindings (вместо xrSuggestInteractionProfileBindings)
Структуры:      XrActionCreateInfo     += поле XrActionSet actionSet (до name)
                XrActionSetBindingsInfo { type, next, XrActionSet, int64_t count, const XrBinding* }
                XrBinding              { XrAction action; XrActionType actionType; XrPath binding; }
Значения:       XrActionType { BOOLEAN=0, FLOAT=1, VECTOR2F=2, POSE=3 }
                XR_TYPE_ACTION_SET_BINDINGS_INFO = 116
Остальное (28/29/38/42/58/60/61, layout action states, xrLocateSpace, xrSyncInputs,
xrUpdateActionState, XrActionSpaceCreateInfo) — совпадает.
```

## Приложение B. Проверенные input-пути (OpenXR 1.0, standard controller)

```
/user/input/right/pose                     /user/input/left/pose
/user/input/{r,l}/gamepad/trigger|squeeze|grip
/user/input/{r,l}/gamepad/thumbstick       /user/input/{r,l}/gamepad/thumbstick/click
/user/input/{r,l}/a|b|x|y                  /user/input/{r,l}/dpad/up|down|left|right
профиль: /interaction_profiles/khr/simple_controller
```
