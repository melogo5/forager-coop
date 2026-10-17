// World sync, phase 1: natural resources and plants.
//   parNatural: trees, rocks/ores (ore type = image_index), crystals, cacti, graves, ...
//   parHerb:    bushes, flowers, wheat, beet, cotton, mushrooms, pumpkins, saplings, ...
//
// Model: the client plays on a copy of the host's save, so islands and terrain match.
//  - The host is the only one who spawns resources; the client's own random spawns are removed.
//  - On HELLO (and after the client changes room) the host sends a snapshot; the client replaces
//    its resources with the host's ones. Every synced resource carries `coNetId`.
//  - Whoever breaks/harvests a resource gets drops and XP; the other side removes it silently.
//  - Plants fire no OnResourceDestroy, so their removal is detected by polling TrueInstanceExists
//    (plants are registered in the TrueInstance system, which also sees culled instances).
//
// Packets (u8 type first):
//   10 SNAPSHOT_BEGIN
//   11 RES       u32 netId, u16 object, f32 x, f32 y, u8 imageIndex, s8 flip  (snapshot entry)
//   12 SNAPSHOT_END u32 count
//   13 SPAWN     same payload as 11 (live spawn on the host)
//   14 DESTROY   u32 netId
//   15 SYNC_REQUEST (client -> host)

#define CoWorldInit
  global.coNetMap = ds_map_create();      // netId -> instance
  global.coKillList = ds_list_create();   // client: own random spawns to remove next step
  global.coSpawnQueue = ds_list_create(); // host: new spawns to send next step (fully set up)
  global.coHerbIds = ds_list_create();    // netIds of synced plants, polled for removal
  global.coNextNetId = 1;
  global.coApplying = false;              // true while we change the world ourselves
  global.coWorldReady = false;            // client: snapshot received
  global.coHerbPollFrames = 10;
  // instance_activate_object only takes effect next step, so snapshots are two-phase
  global.coSnapshotAt = -1;       // host: frame to send the snapshot at
  global.coSnapshotDelay = 2;
  global.coWipePending = false;   // client: remove local (non-host) instances next step
  CoMobInit();

#define CoWorldReset
  if (!variable_global_exists("coNetMap")) exit;  // recompiling over a build without world sync
  ds_map_clear(global.coNetMap);
  ds_list_clear(global.coKillList);
  ds_list_clear(global.coSpawnQueue);
  ds_list_clear(global.coHerbIds);
  CoMobReset();
  global.coWorldReady = false;

#define CoWorldActive
  return global.coSocket >= 0 && !global.coLoopMode && room == rmGame;

#define CoIsSynced(_obj)
  return object_is_ancestor(_obj, parNatural) || object_is_ancestor(_obj, parHerb)
    || object_is_ancestor(_obj, parEnemy);

#define CoRegister(_id, _inst)
  _inst.coNetId = _id;
  ds_map_set(global.coNetMap, _id, _inst);
  if (object_is_ancestor(_inst.object_index, parEnemy)) {
    CoMobTrack(_id);
  } else if (object_is_ancestor(_inst.object_index, parHerb)
      && ds_list_find_index(global.coHerbIds, _id) < 0) {
    ds_list_add(global.coHerbIds, _id);
  }

// ---------- sending ----------

// Existing ids are re-registered: the map is cleared on room load while instances keep coNetId.
#define CoNetIdOf(_inst)
  var _id;
  if (variable_instance_exists(_inst, "coNetId")) {
    _id = _inst.coNetId;
  } else {
    _id = global.coNextNetId;
    global.coNextNetId += 1;
  }
  CoRegister(_id, _inst);
  return _id;

#define CoSendResource(_type, _inst)
  var _buf = buffer_create(24, buffer_grow, 1);
  buffer_write(_buf, buffer_u8, _type);
  buffer_write(_buf, buffer_u32, CoNetIdOf(_inst));
  buffer_write(_buf, buffer_u16, _inst.object_index);
  buffer_write(_buf, buffer_f32, _inst.x);
  buffer_write(_buf, buffer_f32, _inst.y);
  buffer_write(_buf, buffer_u8, floor(_inst.image_index));
  buffer_write(_buf, buffer_s8, sign(_inst.image_xscale));
  CoSendBuffer(_buf);

#define CoSendNetId(_type, _id)
  var _buf = buffer_create(8, buffer_grow, 1);
  buffer_write(_buf, buffer_u8, _type);
  buffer_write(_buf, buffer_u32, _id);
  CoSendBuffer(_buf);

#define CoWorldActivateSynced
  instance_activate_object(parNatural);
  instance_activate_object(parHerb);
  instance_activate_object(parEnemy);

// Host: wake culled instances now, send the snapshot once they are really active.
#define CoWorldRequestSnapshot
  if (!global.coIsHost || !CoWorldActive()) exit;
  CoWorldActivateSynced();
  global.coSnapshotAt = global.coFrame + global.coSnapshotDelay;

// Host: send every synced instance of the current world (called by CoWorldStep after the wake-up;
// the game's culling puts far instances back to sleep on its next pass).
#define CoWorldSendSnapshot
  if (!CoWorldActive()) exit;
  ds_list_clear(global.coHerbIds);
  CoMobReset();
  var _begin = buffer_create(1, buffer_grow, 1);
  buffer_write(_begin, buffer_u8, 10);
  CoSendBuffer(_begin);
  var _count = 0;
  with (parNatural) {
    CoSendResource(11, id);
    _count += 1;
  }
  with (parHerb) {
    CoSendResource(11, id);
    _count += 1;
  }
  with (parEnemy) {
    CoSendMob(id);
    _count += 1;
  }
  CoSendNetId(12, _count);
  Trace("[Coop] world snapshot sent: " + string(_count) + " resources");

// ---------- receiving ----------

#define CoWorldHandle(_type, _buf)
  switch (_type) {
    case 10: CoWorldBeginSnapshot(); break;
    case 11:
    case 13: CoWorldSpawnFromPacket(_buf); break;
    case 12:
      global.coWorldReady = true;
      Trace("[Coop] world synced with host: " + string(buffer_read(_buf, buffer_u32)) + " resources");
      break;
    case 14: CoWorldDestroyById(buffer_read(_buf, buffer_u32)); break;
    case 15: CoWorldRequestSnapshot(); break;
    case 20:
    case 21:
    case 22:
    case 23: CoMobHandle(_type, _buf); break;
  }

// Client: the host's instances arrive next. Local ones are woken up now and removed next step
// (CoWorldWipeLocal), when they are really active; host instances are told apart by coNetId.
#define CoWorldBeginSnapshot
  if (room != rmGame) exit;
  CoWorldActivateSynced();
  global.coWipePending = true;
  ds_map_clear(global.coNetMap);
  ds_list_clear(global.coKillList);
  ds_list_clear(global.coHerbIds);
  CoMobReset();

#define CoWorldSpawnFromPacket(_buf)
  var _id = buffer_read(_buf, buffer_u32);
  var _obj = buffer_read(_buf, buffer_u16);
  var _x = buffer_read(_buf, buffer_f32);
  var _y = buffer_read(_buf, buffer_f32);
  var _img = buffer_read(_buf, buffer_u8);
  var _flip = buffer_read(_buf, buffer_s8);
  if (room != rmGame || ds_map_exists(global.coNetMap, _id) || !object_exists(_obj)) exit;
  global.coApplying = true;
  var _inst = instance_create_depth(_x, _y, -_y, _obj);
  _inst.image_index = _img;
  if (_flip != 0) _inst.image_xscale = abs(_inst.image_xscale) * _flip;
  // plants register in the collision grid in their own Create; natural resources don't
  if (object_is_ancestor(_obj, parNatural)) with (_inst) SolidInit();
  global.coApplying = false;
  CoRegister(_id, _inst);

// Remove without the Destroy event: no drops, no XP, no OnResourceDestroy echo.
// CleanUp still runs, so the game frees the collision grid and rebuilds paths itself.
#define CoWorldDestroyById(_id)
  var _inst = ds_map_find_value(global.coNetMap, _id);
  ds_map_delete(global.coNetMap, _id);
  var _herbPos = ds_list_find_index(global.coHerbIds, _id);
  if (_herbPos >= 0) ds_list_delete(global.coHerbIds, _herbPos);
  CoMobUntrack(_id);
  if (is_undefined(_inst)) exit;
  instance_activate_object(_inst);
  if (!instance_exists(_inst)) exit;
  global.coApplying = true;
  instance_destroy(_inst, false);
  global.coApplying = false;

// ---------- game events ----------

#define OnResourceSpawn(_inst)
  if (!CoWorldActive() || global.coApplying) exit;
  // The event fires before the spawner finishes setting the instance up (e.g. the ore type),
  // so both sides act on it next step.
  if (global.coIsHost) ds_list_add(global.coSpawnQueue, _inst);
  else ds_list_add(global.coKillList, _inst);

#define OnResourceDestroy(_inst)
  if (!CoWorldActive() || global.coApplying) exit;
  if (!variable_instance_exists(_inst, "coNetId")) exit;
  var _id = _inst.coNetId;
  ds_map_delete(global.coNetMap, _id);
  CoSendNetId(14, _id);

// Called from OnRoomLoad in Main.gml.
#define CoWorldRoomLoad
  if (!CoWorldActive()) exit;
  // instances in rmGame are recreated from the save after leaving it: refresh ids on both sides
  if (global.coIsHost) {
    ds_map_clear(global.coNetMap);
    CoWorldRequestSnapshot();
  } else {
    CoSendNetId(15, 0);
  }

// ---------- per-step work (called from OnStep) ----------

#define CoWorldStep
  if (!CoWorldActive()) exit;
  if (global.coSnapshotAt >= 0 && global.coFrame >= global.coSnapshotAt) {
    global.coSnapshotAt = -1;
    CoWorldSendSnapshot();
  }
  if (global.coWipePending) CoWorldWipeLocal();
  CoWorldFlushSpawns();
  CoWorldFlushKills();
  if (global.coFrame mod global.coHerbPollFrames == 0) CoWorldPollHerbs();
  CoMobStep();

#define CoWorldFlushSpawns
  var _n = ds_list_size(global.coSpawnQueue);
  for (var _i = 0; _i < _n; _i += 1) {
    var _inst = ds_list_find_value(global.coSpawnQueue, _i);
    if (TrueInstanceExists(_inst)) {
      instance_activate_object(_inst);
      if (object_is_ancestor(_inst.object_index, parEnemy)) CoSendMob(_inst);
      else if (CoIsSynced(_inst.object_index)) CoSendResource(13, _inst);
    }
  }
  ds_list_clear(global.coSpawnQueue);

// Client: remove every synced-type instance that did not come from the host.
#define CoWorldWipeLocal
  global.coWipePending = false;
  global.coApplying = true;
  var _removed = 0;
  with (parNatural) if (!variable_instance_exists(id, "coNetId")) { instance_destroy(id, false); _removed += 1; }
  with (parHerb) if (!variable_instance_exists(id, "coNetId")) { instance_destroy(id, false); _removed += 1; }
  with (parEnemy) if (!variable_instance_exists(id, "coNetId")) { instance_destroy(id, false); _removed += 1; }
  global.coApplying = false;
  Trace("[Coop] removed " + string(_removed) + " local instances");

#define CoWorldFlushKills
  var _n = ds_list_size(global.coKillList);
  if (_n == 0) exit;
  global.coApplying = true;
  for (var _i = 0; _i < _n; _i += 1) {
    var _inst = ds_list_find_value(global.coKillList, _i);
    instance_activate_object(_inst);
    if (instance_exists(_inst) && !variable_instance_exists(_inst, "coNetId")) {
      instance_destroy(_inst, false);
    }
  }
  global.coApplying = false;
  ds_list_clear(global.coKillList);

// Harvested plants just disappear (no mod event): report the ones that are gone.
#define CoWorldPollHerbs
  for (var _i = ds_list_size(global.coHerbIds) - 1; _i >= 0; _i -= 1) {
    var _id = ds_list_find_value(global.coHerbIds, _i);
    var _inst = ds_map_find_value(global.coNetMap, _id);
    if (is_undefined(_inst) || !TrueInstanceExists(_inst)) {
      ds_list_delete(global.coHerbIds, _i);
      if (!is_undefined(_inst)) {
        ds_map_delete(global.coNetMap, _id);
        CoSendNetId(14, _id);
      }
    }
  }
