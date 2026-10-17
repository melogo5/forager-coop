// World sync, phase 2: enemies (parEnemy: slimes, skeletons, boars, demons, ...).
//
// Only the host spawns enemies (the client's own spawns are removed, as with resources), but each
// enemy is *simulated* by its owner: the player standing closer to it. The game's enemy AI only
// ever chases its own local player, so this is what lets enemies attack either player.
//  - The host assigns owners (0 = host, 1 = client) with hysteresis and announces changes.
//  - The owner streams position and HP of its enemies 10 times a second; the other side pulls
//    them towards that position at the end of every step, after their local AI has moved them.
//  - HP only goes down: both sides keep min(local, remote), so hits of both players add up; hits
//    on an enemy someone else owns are reported to the other side right away.
//  - Whoever lands the killing blow gets drops and XP; the other side removes the enemy silently.
//  - On the host synced enemies never fall asleep off-screen (antiCull), so the client's enemies
//    keep living there.
//
// Packets (u8 type first; netIds share one counter with resources):
//   20 MOB        u32 netId, u16 object, f32 x, f32 y, f32 hp   (snapshot entry or live spawn)
//   21 MOB_STATE  u16 count, then count x (u32 netId, f32 x, f32 y, f32 hp)   owner -> other
//   22 MOB_HP     u32 netId, f32 hp                               non-owner -> owner
//   23 MOB_OWNER  u32 netId, u8 owner                             host -> client
//   14 DESTROY    (World.gml) is reused for deaths

#define CoMobInit
  global.coMobIds = ds_list_create();
  global.coMobStateFrames = 6;   // owner -> other stream, 10 Hz
  global.coMobHpPollFrames = 3;  // damage reports for enemies someone else owns
  global.coMobOwnerFrames = 30;  // host re-evaluates owners twice a second
  global.coMobOwnerBias = 0.75;  // switch owner only when the other player is 25% closer
  global.coMobFollow = 0.35;     // non-owner: share of the gap closed each step

#define CoMobReset
  if (!variable_global_exists("coMobIds")) exit;
  ds_list_clear(global.coMobIds);

#define CoMobTrack(_id)
  if (ds_list_find_index(global.coMobIds, _id) < 0) ds_list_add(global.coMobIds, _id);

#define CoMobUntrack(_id)
  var _pos = ds_list_find_index(global.coMobIds, _id);
  if (_pos >= 0) ds_list_delete(global.coMobIds, _pos);

#define CoMyRole
  return global.coIsHost ? 0 : 1;

#define CoMobOwner(_inst)
  return variable_instance_exists(_inst, "coOwner") ? _inst.coOwner : 0;

// Live tracked enemy instance for list position _i, or noone (asleep or gone).
#define CoMobAt(_i)
  var _inst = ds_map_find_value(global.coNetMap, ds_list_find_value(global.coMobIds, _i));
  if (is_undefined(_inst) || !instance_exists(_inst)) return noone;
  return _inst;

// ---------- sending ----------

#define CoSendMob(_inst)
  var _id = CoNetIdOf(_inst);
  if (global.coIsHost) _inst.antiCull = true;
  _inst.coLastHp = _inst.hp;
  var _buf = buffer_create(24, buffer_grow, 1);
  buffer_write(_buf, buffer_u8, 20);
  buffer_write(_buf, buffer_u32, _id);
  buffer_write(_buf, buffer_u16, _inst.object_index);
  buffer_write(_buf, buffer_f32, _inst.x);
  buffer_write(_buf, buffer_f32, _inst.y);
  buffer_write(_buf, buffer_f32, _inst.hp);
  CoSendBuffer(_buf);

// One packet with every live, active enemy this side owns.
#define CoMobSendState
  var _live = ds_list_create();
  var _me = CoMyRole();
  for (var _i = ds_list_size(global.coMobIds) - 1; _i >= 0; _i -= 1) {
    var _id = ds_list_find_value(global.coMobIds, _i);
    var _inst = ds_map_find_value(global.coNetMap, _id);
    if (is_undefined(_inst) || !TrueInstanceExists(_inst)) {
      ds_list_delete(global.coMobIds, _i);  // gone without a death event
    } else if (instance_exists(_inst) && CoMobOwner(_inst) == _me) {
      ds_list_add(_live, _id);
    }
  }
  var _count = ds_list_size(_live);
  if (_count > 0) {
    var _buf = buffer_create(4 + _count * 16, buffer_grow, 1);
    buffer_write(_buf, buffer_u8, 21);
    buffer_write(_buf, buffer_u16, _count);
    for (var _i = 0; _i < _count; _i += 1) {
      var _id = ds_list_find_value(_live, _i);
      var _inst = ds_map_find_value(global.coNetMap, _id);
      buffer_write(_buf, buffer_u32, _id);
      buffer_write(_buf, buffer_f32, _inst.x);
      buffer_write(_buf, buffer_f32, _inst.y);
      buffer_write(_buf, buffer_f32, _inst.hp);
      _inst.coLastHp = _inst.hp;
    }
    CoSendBuffer(_buf);
  }
  ds_list_destroy(_live);

// Report HP lost to our hits on enemies the other side owns.
#define CoMobPollDamage
  var _me = CoMyRole();
  for (var _i = ds_list_size(global.coMobIds) - 1; _i >= 0; _i -= 1) {
    var _inst = CoMobAt(_i);
    if (_inst == noone || CoMobOwner(_inst) == _me) continue;
    if (_inst.hp < _inst.coLastHp) {
      _inst.coLastHp = _inst.hp;
      var _buf = buffer_create(12, buffer_grow, 1);
      buffer_write(_buf, buffer_u8, 22);
      buffer_write(_buf, buffer_u32, _inst.coNetId);
      buffer_write(_buf, buffer_f32, _inst.hp);
      CoSendBuffer(_buf);
    }
  }

// Host: give each enemy to the closer player, with hysteresis.
#define CoMobAssignOwners
  if (!global.coRemoteSeen || !instance_exists(objPlayer)) exit;
  var _hx = objPlayer.x;
  var _hy = objPlayer.y;
  for (var _i = ds_list_size(global.coMobIds) - 1; _i >= 0; _i -= 1) {
    var _inst = CoMobAt(_i);
    if (_inst == noone) continue;
    var _toHost = point_distance(_inst.x, _inst.y, _hx, _hy);
    var _toClient = point_distance(_inst.x, _inst.y, global.coRemoteX, global.coRemoteY);
    var _owner = CoMobOwner(_inst);
    var _next = _owner;
    if (_owner == 0 && _toClient < _toHost * global.coMobOwnerBias) _next = 1;
    if (_owner == 1 && _toHost < _toClient * global.coMobOwnerBias) _next = 0;
    if (_next == _owner) continue;
    _inst.coOwner = _next;
    _inst.coTargetX = _inst.x;  // no jump to a stale target before the new owner's first update
    _inst.coTargetY = _inst.y;
    var _buf = buffer_create(8, buffer_grow, 1);
    buffer_write(_buf, buffer_u8, 23);
    buffer_write(_buf, buffer_u32, _inst.coNetId);
    buffer_write(_buf, buffer_u8, _next);
    CoSendBuffer(_buf);
  }

// ---------- receiving ----------

#define CoMobHandle(_type, _buf)
  switch (_type) {
    case 20: CoMobSpawnFromPacket(_buf); break;
    case 21: CoMobApplyState(_buf); break;
    case 22:
      var _id = buffer_read(_buf, buffer_u32);
      CoMobLowerHp(ds_map_find_value(global.coNetMap, _id), buffer_read(_buf, buffer_f32));
      break;
    case 23:
      var _id = buffer_read(_buf, buffer_u32);
      var _owner = buffer_read(_buf, buffer_u8);
      var _inst = ds_map_find_value(global.coNetMap, _id);
      if (!is_undefined(_inst) && instance_exists(_inst)) {
        _inst.coOwner = _owner;
        _inst.coTargetX = _inst.x;
        _inst.coTargetY = _inst.y;
      }
      break;
  }

#define CoMobSpawnFromPacket(_buf)
  var _id = buffer_read(_buf, buffer_u32);
  var _obj = buffer_read(_buf, buffer_u16);
  var _x = buffer_read(_buf, buffer_f32);
  var _y = buffer_read(_buf, buffer_f32);
  var _hp = buffer_read(_buf, buffer_f32);
  if (room != rmGame || ds_map_exists(global.coNetMap, _id) || !object_exists(_obj)) exit;
  global.coApplying = true;
  var _inst = instance_create_depth(_x, _y, -_y, _obj);
  global.coApplying = false;
  _inst.hp = min(_inst.hp, _hp);
  _inst.coLastHp = _inst.hp;
  _inst.coTargetX = _x;
  _inst.coTargetY = _y;
  _inst.coOwner = 0;
  CoRegister(_id, _inst);

#define CoMobApplyState(_buf)
  var _count = buffer_read(_buf, buffer_u16);
  for (var _i = 0; _i < _count; _i += 1) {
    var _id = buffer_read(_buf, buffer_u32);
    var _x = buffer_read(_buf, buffer_f32);
    var _y = buffer_read(_buf, buffer_f32);
    var _hp = buffer_read(_buf, buffer_f32);
    var _inst = ds_map_find_value(global.coNetMap, _id);
    if (is_undefined(_inst) || !instance_exists(_inst)) continue;
    _inst.coTargetX = _x;
    _inst.coTargetY = _y;
    CoMobLowerHp(_inst, _hp);
  }

#define CoMobLowerHp(_inst, _hp)
  if (is_undefined(_inst) || !instance_exists(_inst)) exit;
  if (_hp < _inst.hp) _inst.hp = _hp;
  _inst.coLastHp = _inst.hp;

// ---------- per-step ----------

// Called from CoWorldStep.
#define CoMobStep
  if (global.coFrame mod global.coMobStateFrames == 0) CoMobSendState();
  if (global.coFrame mod global.coMobHpPollFrames == 0) CoMobPollDamage();
  if (global.coIsHost && global.coFrame mod global.coMobOwnerFrames == 0) CoMobAssignOwners();

// Enemies owned by the other side: after their local AI has moved them, pull them to where the
// owner has them.
#define OnStepEnd
  if (!CoWorldActive()) exit;
  var _me = CoMyRole();
  for (var _i = ds_list_size(global.coMobIds) - 1; _i >= 0; _i -= 1) {
    var _inst = CoMobAt(_i);
    if (_inst == noone || CoMobOwner(_inst) == _me) continue;
    if (!variable_instance_exists(_inst, "coTargetX")) continue;
    _inst.x += (_inst.coTargetX - _inst.x) * global.coMobFollow;
    _inst.y += (_inst.coTargetY - _inst.y) * global.coMobFollow;
  }

// ---------- game events ----------

#define OnMobSpawn(_inst)
  if (!CoWorldActive() || global.coApplying) exit;
  // fires from Create, before the spawner finishes: act next step, like resources
  if (global.coIsHost) ds_list_add(global.coSpawnQueue, _inst);
  else ds_list_add(global.coKillList, _inst);

#define OnMobDeath(_inst)
  if (!CoWorldActive() || global.coApplying) exit;
  if (!variable_instance_exists(_inst, "coNetId")) exit;
  var _id = _inst.coNetId;
  ds_map_delete(global.coNetMap, _id);
  CoMobUntrack(_id);
  CoSendNetId(14, _id);
