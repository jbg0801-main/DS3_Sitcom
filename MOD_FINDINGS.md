# Mod findings

This is the hook recipe for a DLL. `FINDINGS.md` is the evidence behind each address. The Ghidra database has the same names on `DarkSoulsIII_dump.exe`.

Image base `0x140000000`. Windows x64 `__fastcall`: `RCX`, `RDX`, `R8`, `R9`, then the stack. Vtable methods have no direct callers, so scan the prologue (AOB) or hook the vtable slot.

Local player: `Chr_IsLocalPlayer` at `0x14088CFF0` is true when `chr == *(g_WorldChrMan + 0x80)`. `g_WorldChrMan` is `0x14477FDB8`. Character HP lives on the module hub: `chr + 0x1F90`, then `+0x18` is `ChrDataModule`, and HP is `+0xD8` on that module.

```cpp
using Chr_IsLocalPlayer_fn = uint8_t(__fastcall*)(void* chr);
```

---

## Getting hit

Hook `ChrDamageModule_ApplyDamageToHp` at `0x1409C48C0`. This is the only path that subtracts HP from an attack packet. It calls `ChrData_ApplyHpDelta(data, -applied_damage, 1)` and does not call `Chr_Kill`.

```cpp
using ApplyDamageToHp_fn = void(__fastcall*)(
    void* damage_module,   // +8 = chr
    void* damage_info,
    int32_t max_damage,
    int32_t applied_damage,
    uint8_t a5,
    uint8_t a6);
```

Read `chr` from `*(void**)(damage_module + 8)` and keep the call when `Chr_IsLocalPlayer(chr)` is set. `applied_damage` is the amount taken off HP. HP before the call and HP after the call tells you whether the hit killed: this function leaves a killing blow as HP crossing 0, and the death sequence is a later call.

Leave these on other hooks. They also change HP, and they do not come through `ApplyDamageToHp`:

| What you felt | Where it actually is |
|---------------|----------------------|
| Poison, toxic, or any other repeating behavior tick | Behavior event **1000** on `Chr_OnBehaviorEvent` (`0x140996760`) or `ChrCtrl_OnBehaviorEvent` (`0x1405D7AB0`). Delta is applied with `is_attack_damage == 0`. Event 1000 is every scripted HP change, so a positive delta is a heal. |
| Fall damage | `ChrDamageModule_ApplyFallDamage` (`0x1409C7CE0`) |
| Max-HP change copying the current HP up | `ApplyHpDelta` caller `0x140B06C9B` |

A hook on `ChrData_ApplyHpDelta` (`0x1409E36D0`) sees all of the above. Its third argument is `1` only for `ApplyDamageToHp`.

---

## Dying

Hook `Chr_Kill` at `0x140888FC0`.

```cpp
using ChrKill_fn = void(__fastcall*)(void* chr, uint8_t death_kind);
```

Filter with `Chr_IsLocalPlayer(chr)`. The function sets HP to 0, sets `chr + 0x1A0A` bit 1, stores `death_kind` at `chr + 0x1B94`, then runs `Chr_ProcessPendingDeath` (`0x140890E00`).

`death_kind` is the cause the killer passed in:

| `death_kind` | Who called |
|-------------:|------------|
| 0 | `ChrDamageModule_ForceKillFromFall` (`0x1409C8280`), debug command 6, and `WorldChrMan_SyncSessionHp` (`0x1408DB570`) when a session HP word hits 0 |
| 1 | `WorldChrMan_ApplyRemoteDamage` (`0x1408DEF10`) and `ChrSync_ApplyReplicatedVitals` (`0x1405DB7D0`) when a replicated HP word lands on 0 |

A normal attack that reduces HP to 0 does not call `Chr_Kill` itself. Pair this hook with the hit hook: HP crossing 0 inside `ApplyDamageToHp` is the killing blow, and `Chr_Kill` is the death sequence. A lethal fall posts request type 3 inside `ApplyFallDamage` and only reaches `Chr_Kill` through `ForceKillFromFall` (`death_kind` 0).

The You Died banner is text effect **2** (`F23_YouDied`), queued through `Event_SetTextEffect` (`0x140486190`) into `MapName_QueueMenuEvent`. The widget table is `TextEffectWidgetNames` at `0x14458C4C0`. That banner is the menu, not the moment HP hits 0.

---

## Drinking from an empty Estus flask

Hook `ChrCtrl_UseItem` at `0x1405BA4C0`. AOB scan. Callers are action opcode `0x41` and a debug path. `chr_action_ctrl` is `chr + 0x10E8`.

```cpp
using ChrCtrl_UseItem_fn = uint8_t(__fastcall*)(
    void* chr_action_ctrl,
    int goodsId,
    uint32_t* use_item_info,
    int slot_or_flags,
    char start_behavior);
```

Classify `goodsId` from its `EquipParamGoods` row:

| Check | Goods row | Result |
|-------|-----------|--------|
| `+0x69` `SuppleItemType` is 1 | HP Estus | |
| `+0x69` is 2 | Ashen Estus | |
| `+0x45` bit 7 `isSuppleItem` | Empty flask. Shake anim `a000_050114` | The empty drink |
| `+0x46` bit 0 `isFullSuppleItem` | Charged flask | A real drink |

On the shipped flask rows the empty ids are the evens (`150/152/…`, `190/192/…`) and the charged ids are the odds (`151/153/…`, `191/193/…`). The bit flags are the check. `use_item_info[2] == 0` is copied to the request at `+0x1C` and is the empty flag on that request.

`ChrEstusEatSlot_TryConsume` (`0x1405B1370`) only adds multiplayer bonus charges. It is not the drink. Charge counts at the moment of the press are `Chr_GetHpEstusCount` (`0x1408BBCD0`) and `Chr_GetMpEstusCount` (`0x1408BBD10`).

---

## A boss fight starting

The health bar appearing is the start signal. Spawning the enemy is earlier and is the same path ordinary event enemies use.

Hook `Event_SetBossGauge` at `0x1404855A0`, or the write it calls, `BossGauge_SetSlot` at `0x140475470`.

```cpp
using Event_SetBossGauge_fn = void(__fastcall*)(
    void* event, int32_t entity_id, int32_t slot, int32_t name_msg_id);
```

Three slots live at `g_MapNameState + 0x1AF8` (`g_MapNameState` is `0x144763258`), stride `0x98`. `BossGauge_GetSlot` (`0x140761500`) returns slot `0..2`. The bar is on when both of these are not `-1`:

| Slot offset | Field |
|------------:|-------|
| `+0x00` | Boss-name message id |
| `+0x04` | Chr handle (`chr + 8`) |

`FrpgMenuDlgBossGauge_Update` (`0x1406CB180`) is the menu side. Dialog `+0x170` is the slot index. It activates the dialog once both dwords are valid. Vftable `0x1427F5588`, update at slot `0x80`.

`Event_RequestGenerate` (`0x140485060`) and `EnemyIns_Spawn` (`0x1408A9B40`) run when the chr is created. Treat those as the spawn. Treat `SetBossGauge` as the fight's health bar. `EnemyIns_Spawn` AOB: `40 53 55 56 57 41 54 41 56 41 57 48 81 EC 80 00`.

---

## A boss fight ending

"The boss died" and "the health bar was cleared" are different calls. Clear-all also runs when a fight is failed, when a block is cleared, and when the event runtime is created.

**The boss died.** On `Chr_Kill`, or when HP crosses 0 inside `ApplyDamageToHp`, read `chr + 8` and compare it to each gauge slot's handle at `+0x04`. A match is the boss whose bar is on screen. `ApplyDamageToHp` still does not call `Chr_Kill` for that killing blow.

**The fight script ended.** Hook `Event_MediumBossDestroy` at `0x140491DF0`. It clears every gauge slot, then queues text effect **1** (`F23_DemonDestroy`, caption `VICTORY ACHIEVED`) when `args + 8` is not 1, `Event_GetTempSummonParam` (`0x140482110`) is below 1, and the player is live, grey, white, black, or an intruder. The loot call is not inside this function. Boss loot is `Event_GetRateItem` (`0x1404807E0`) or `Event_AddInventoryItem` (`0x14047D0B0`), and the popup is `ItemGet_WritePopupRequest` (`0x1407651C0`).

These also clear every slot, without being the boss's death:

| Address | Name |
|---------|------|
| `0x14075E060` | `BossGauge_ClearAllSlots` (thunk `Event_ClearBossGauge` at `0x14047E030`) |
| `0x14048EE70` | `Event_BlockClear2` |
| `0x1404901E0` | `Event_FailedBossAreaMission` |
| `0x1404827C0` | `EventRuntime_Reset` |

---

## Entering an area

The title card is a place-name change from the floor you are standing on. A map load does not have to happen.

Hook `MapName_QueueMenuEvent` at `0x140765F30` and keep `menu_event_id == 8`. That is the title card. The ring is `g_MapNameState + 0x19D0`, index `+0x1A2C`. `g_MapNameState` is `0x144763258`.

`MapNameState_Update` (`0x140767AC0`) is the frame that decides to show it. It reads the floor hit at `player_chr + 0x1AC8`, gets the place id from `HitParts_GetMapNameId` (`0x140857B10`), and on a change versus the last shown id at `g_MapNameState + 0x1A3C` writes the new id to `g_MapNameState + 0x1A38`. After that call, `*(uint32_t*)(g_MapNameState + 0x1A38)` is the place id the card is for. Same-map titles (Firelink, Cemetery of Ash) take this path.

The widget update is `FrpgMenuDlgTE_MapName_UpdatePlaceName` at `0x14076E660`. It is a vtable method. AOB: `40 55 48 81 EC 80 00 00 00 0F 29 74 24 40 48 8B 05 1B 70 FB 03`. Vftable `0x14281CB58`.

```cpp
using UpdatePlaceName_fn = void(__fastcall*)(void* self, float deltaTime);
```

Text effect **8** (`F23_MapName`) is the same card on the text-effect widget list. Menu event 8 is the listen point for "the title card was queued."

---

## Falling off a ladder

Hook `Chr_OnBehaviorEvent` (`0x140996760`) and `ChrCtrl_OnBehaviorEvent` (`0x1405D7AB0`) for event id **`0x85` (133)**. Argument index 2 is the new ladder act state. Those two functions are the only C++ writers of the act byte.

```cpp
using OnBehaviorEvent_fn = void(__fastcall*)(
    void* event_target, int32_t event_id, void* event_args);
```

| New state | What happened |
|----------:|---------------|
| 21 | Hit off the ladder |
| 22 | Heavy hit off the ladder |
| 23 | Fell because stamina ran out |
| 14–20 | The player chose to slide |
| -1 | Not on a ladder |

The byte itself is at `*(*(chr + 0x50) + 0x48) + 0x2CC`. The ladder module is `*(chr + 0x1F90) + 0x78`. `ChrLadder_IsOnLadder` (`0x1409D3690`) stays true through the slide and through states 21–23, so it does not mean "fell off."

Scripted climb events are separate: `OnEvent_LadderUp` is 5000 and `OnEvent_LadderDawn` is `0x1392` (5010). They are not the knock-off.
