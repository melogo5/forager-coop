// CoopTest: checks that mods compile, that TCP networking works between two games,
// and draws the other player's position as a first "puppet".
//
// Commands (open chat with T):
//   /cohost [port]       start a server (default port 7777)
//   /cojoin [ip] [port]  connect to a host (default 127.0.0.1:7777)
//   /coloop [port]       self-test in one game: host + client over 127.0.0.1;
//                        the host side echoes your position 48px to the right
//   /costatus            print connection state
//   /costop              close all sockets

#define Main
  // COMPILE re-runs Main without unloading: close sockets left open, or the port stays busy.
  if (variable_global_exists("coServer")) CoClose();
  // Going to the main menu unloads mods completely and wipes their globals. What must survive
  // that trip (a pending reconnect and the banner explaining it) is carried in the game's own
  // globals, which live as long as the game process.
  var _keepReconnect = false;
  var _keepIp = "127.0.0.1";
  var _keepPort = 7777;
  var _keepBanner = "";
  if (variable_global_exists("coopCarryReconnect")) {
    _keepReconnect = variable_global_get("coopCarryReconnect");
    _keepIp = variable_global_get("coopCarryIp");
    _keepPort = variable_global_get("coopCarryPort");
    _keepBanner = variable_global_get("coopCarryBanner");
    variable_global_set("coopCarryReconnect", false);
    variable_global_set("coopCarryBanner", "");
  }
  var _keepBannerFrames = _keepBanner != "" ? 300 : 0;
  global.coPort = 7777;
  global.coHelloTimeoutFrames = 180;  // 3 s at 60 fps
  global.coServer = -1;    // listening socket (host only)
  global.coSocket = -1;     // socket to the other side (host: accepted client, client: own socket)
  global.coIsHost = false;
  global.coLoopPeer = -1;   // loop mode: server-side socket of our own client connection
  global.coLoopMode = false;
  global.coFrame = 0;
  global.coPingsSent = 0;
  global.coPongs = 0;
  global.coRemoteX = 0;
  global.coRemoteY = 0;
  global.coRemoteSeen = false;
  global.coHelloSeen = false;
  global.coConnectFrame = 0;
  global.coPuppet = noone;  // never -1: in GML -1 means `self`
  global.coBannerText = "";
  global.coBannerFrames = 0;
  global.coBannerDuration = 300;  // 5 s at 60 fps
  global.coBannerFade = 40;
  // islands: the game can't build a bought island on the client, so the client reloads the
  // host's freshly saved world and reconnects by itself
  global.coLastIp = "127.0.0.1";
  global.coReconnectPending = false;
  global.coReconnectAt = -1;
  global.coReconnectDelayFrames = 60;
  global.coBuyLandCoins = -1;         // host: coins seen on the Buy land screen
  global.coLandPendingSince = -1;     // host: GUI frame when a purchase was noticed
  global.coLandSaveWaitFrames = 180;  // send RELOAD even if OnSaveEnd never comes
  // a crashed peer sends no disconnect event: treat silence as a lost connection
  // timing that must survive pauses runs on GUI frames (see OnDrawGUI)
  global.coGuiFrame = 0;
  global.coLastRecvGui = 0;
  global.coSilenceTimeoutFrames = 300;  // 5 s
  global.coKeepaliveFrames = 30;        // a KEEPALIVE every 0.5 s, even while paused
  // After closing sockets the runner still delivers late network events; if they arrive while
  // the game unloads mods on the way to the main menu, it crashes. So wait before going there.
  global.coMenuAt = -1;
  global.coMenuDelayFrames = 90;  // 1.5 s
  global.coReconnectPending = _keepReconnect;
  global.coLastIp = _keepIp;
  global.coPort = _keepPort;
  global.coBannerText = _keepBanner;
  global.coBannerFrames = _keepBannerFrames;
  CoWorldInit();

  CommandCreate("cohost", false, ScriptWrap(CoCmdHost), "port");
  CommandCreate("cojoin", false, ScriptWrap(CoCmdJoin), "ip", "port");
  CommandCreate("coloop", false, ScriptWrap(CoCmdLoop), "port");
  CommandCreate("costatus", false, ScriptWrap(CoCmdStatus));
  CommandCreate("comenu", false, ScriptWrap(CoCmdMenu));  // test helper: back to the main menu
  CommandCreate("costop", false, ScriptWrap(CoCmdStop));
  Trace("[CoopTest] loaded. Commands: /cohost, /cojoin <ip>, /coloop, /costatus, /costop");

// ---------- protocol ----------
// Every packet: u8 type, then payload.
//   1 HELLO  u8 isHost
//   2 PING   u32 frame
//   3 PONG   u32 frame (echoed)
//   4 POS    f32 x, f32 y
//   5 KEEPALIVE  (sent from OnDrawGUI, so it keeps flowing while the game is paused)
//   6 DEBUG  string — shown in the other side's console (a "black box" for crashes)
//   10..15   world sync, see World.gml; 20..23 enemies, see Mobs.gml
//   16 BYE   u8 reason (0 = went to the main menu, 1 = /costop, 2 = reloading the world)
//   17 RELOAD  host -> client: an island was bought and saved, reload the world and reconnect

// Traces locally and in the other side's console, so the trail survives a crash of this game.
#define CoDebug(_text)
  Trace("[CoopTest] " + _text);
  if (global.coSocket < 0) exit;
  var _buf = buffer_create(64, buffer_grow, 1);
  buffer_write(_buf, buffer_u8, 6);
  buffer_write(_buf, buffer_string, _text);
  CoSendBuffer(_buf);

// Sends a filled buffer to the other side and frees it.
#define CoSendBuffer(_buf)
  if (global.coSocket >= 0) network_send_packet(global.coSocket, _buf, buffer_tell(_buf));
  buffer_delete(_buf);

#define CoSend(_type, _a, _b)
  CoSendTo(global.coSocket, _type, _a, _b);

#define CoSendTo(_socket, _type, _a, _b)
  if (_socket < 0) exit;
  var _buf = buffer_create(16, buffer_grow, 1);
  buffer_write(_buf, buffer_u8, _type);
  switch (_type) {
    case 1: buffer_write(_buf, buffer_u8, _a); break;
    case 2:
    case 3: buffer_write(_buf, buffer_u32, _a); break;
    case 4:
      buffer_write(_buf, buffer_f32, _a);
      buffer_write(_buf, buffer_f32, _b);
      break;
  }
  network_send_packet(_socket, _buf, buffer_tell(_buf));
  buffer_delete(_buf);

// Loop mode: packets that arrive on the server side of our own connection.
#define CoHandleLoopPacket(_buf)
  var _type = buffer_read(_buf, buffer_u8);
  switch (_type) {
    case 1: CoSendTo(global.coLoopPeer, 1, 1, 0); break;
    case 2: CoSendTo(global.coLoopPeer, 3, buffer_read(_buf, buffer_u32), 0); break;
    case 4:
      var _x = buffer_read(_buf, buffer_f32);
      var _y = buffer_read(_buf, buffer_f32);
      CoSendTo(global.coLoopPeer, 4, _x + 48, _y);
      break;
  }

#define CoHandlePacket(_buf)
  var _type = buffer_read(_buf, buffer_u8);
  switch (_type) {
    case 1:
      var _peerIsHost = buffer_read(_buf, buffer_u8);
      global.coHelloSeen = true;
      Trace("[CoopTest] HELLO from " + (_peerIsHost ? "host" : "client"));
      if (global.coIsHost) {
        global.coLandPendingSince = -1;
        CoWorldRequestSnapshot();
      } else {
        CoBanner("Connected to the host");
      }
      break;
    case 2:
      CoSend(3, buffer_read(_buf, buffer_u32), 0);
      break;
    case 3:
      var _rtt = global.coFrame - buffer_read(_buf, buffer_u32);
      global.coPongs += 1;
      if (global.coPongs <= 3 || global.coPongs mod 10 == 0) {
        Trace("[CoopTest] pong #" + string(global.coPongs) + ", RTT " + string(_rtt) + " frames");
      }
      break;
    case 4:
      global.coRemoteX = buffer_read(_buf, buffer_f32);
      global.coRemoteY = buffer_read(_buf, buffer_f32);
      if (!global.coRemoteSeen) {
        global.coRemoteSeen = true;
        Trace("[CoopTest] receiving other player's position");
      }
      break;
    case 5:
      break;  // KEEPALIVE: receiving it already refreshed coLastRecvGui
    case 6:
      Trace("[peer] " + buffer_read(_buf, buffer_string));
      break;
    case 16:
      CoPeerGone(true, buffer_read(_buf, buffer_u8));
      break;
    case 17:
      CoReloadForIsland();
      break;
    default:
      if (_type >= 10 && _type <= 23) CoWorldHandle(_type, _buf);
      else Trace("[CoopTest] unknown packet type " + string(_type));
  }

// ---------- commands ----------

#define CoArg(_args, _index, _default)
  if (is_array(_args) && array_length_1d(_args) > _index) {
    var _v = _args[_index];
    if (!is_undefined(_v) && string(_v) != "") return _v;
  }
  return _default;

#define CoCmdHost(_args)
  if (global.coServer >= 0 || global.coSocket >= 0) {
    Trace("[CoopTest] already active, use /costop first");
    exit;
  }
  global.coPort = real(CoArg(_args, 0, 7777));
  global.coServer = network_create_server(network_socket_tcp, global.coPort, 1);
  if (global.coServer < 0) {
    Trace("[CoopTest] could not open port " + string(global.coPort) + " (busy or blocked)");
    exit;
  }
  global.coIsHost = true;
  Trace("[CoopTest] hosting on port " + string(global.coPort) + ", waiting for a player...");

#define CoCmdJoin(_args)
  if (global.coServer >= 0 || global.coSocket >= 0) {
    Trace("[CoopTest] already active, use /costop first");
    exit;
  }
  CoJoin(string(CoArg(_args, 0, "127.0.0.1")), real(CoArg(_args, 1, 7777)));

#define CoJoin(_ip, _port)
  global.coPort = _port;
  global.coLastIp = _ip;
  global.coSocket = network_create_socket(network_socket_tcp);
  Trace("[CoopTest] connecting to " + _ip + ":" + string(global.coPort) + "...");
  if (network_connect(global.coSocket, _ip, global.coPort) < 0) {
    Trace("[CoopTest] connection failed");
    CoBanner("Could not connect to " + _ip + ":" + string(global.coPort));
    network_destroy(global.coSocket);
    global.coSocket = -1;
    exit;
  }
  global.coIsHost = false;
  global.coHelloSeen = false;
  global.coConnectFrame = global.coFrame;
  global.coLastRecvGui = global.coGuiFrame;
  Trace("[CoopTest] connected");
  CoSend(1, 0, 0);

#define CoCmdLoop(_args)
  if (global.coServer >= 0 || global.coSocket >= 0) {
    Trace("[CoopTest] already active, use /costop first");
    exit;
  }
  global.coPort = real(CoArg(_args, 0, 7777));
  global.coServer = network_create_server(network_socket_tcp, global.coPort, 1);
  if (global.coServer < 0) {
    Trace("[CoopTest] loop: could not open port " + string(global.coPort));
    exit;
  }
  Trace("[CoopTest] loop: server on port " + string(global.coPort) + ", connecting to self...");
  global.coSocket = network_create_socket(network_socket_tcp);
  if (network_connect(global.coSocket, "127.0.0.1", global.coPort) < 0) {
    Trace("[CoopTest] loop: connection to self failed");
    CoClose();
    exit;
  }
  global.coIsHost = false;
  global.coLoopMode = true;
  Trace("[CoopTest] loop: client connected");
  CoSend(1, 0, 0);

#define CoCmdStatus(_args)
  Trace("[CoopTest] host=" + string(global.coIsHost) + " server=" + string(global.coServer)
    + " socket=" + string(global.coSocket) + " loopPeer=" + string(global.coLoopPeer)
    + " pings=" + string(global.coPingsSent)
    + " pongs=" + string(global.coPongs) + " remoteSeen=" + string(global.coRemoteSeen));

#define CoCmdMenu(_args)
  if (room == rmMainMenu) exit;
  if (global.coSocket >= 0 || global.coServer >= 0) CoLeaveThenMenu(0);
  else CoGoToMainMenu();

// Close the session now, go to the main menu once late network events have been delivered.
#define CoLeaveThenMenu(_reason)
  CoLeave(_reason);
  CoMenuLater();

#define CoMenuLater
  global.coMenuAt = global.coGuiFrame + global.coMenuDelayFrames;

// objTransition.target is a room id, or something else for special exits. Only gameplay rooms
// keep mods loaded; "Save and exit" goes to rmInputSelect, then rmModCompile recompiles mods.
#define CoTargetUnloadsMods(_target)
  if (!is_real(_target)) return true;
  return !CoIsGameplayRoom(_target);

#define CoIsGameplayRoom(_room)
  return _room == rmGame || _room == rmSkillGrid || _room == rmFeats || _room == rmMuseum
    || _room == rmAncientTomb || _room == rmSkullMaze || _room == rmCrystalCave
    || _room == rmFireTemple || _room == rmDesertSpaceTower || _room == rmGraveyardSpaceTower
    || _room == rmSnowSpaceTower || _room == rmVolcanoSpaceTower || _room == rmWizardTowerInterior
    || _room == rmDruidTreeInterior;

// The game's own "Save and exit" starts an objTransition (the save is already written by then).
// Mods are unloaded only when the game goes to the main menu (or restarts), and a network event
// arriving then breaks the game's mod event dispatch (WorldControl Async Networking ->
// ref_map_wrap: "Data structure with index does not exist", or a hard crash). So any transition
// that may lead there is held: removed, the session closed, our own transition started later.
// Transitions to dungeons and other game rooms keep mods loaded and are left alone.
#define CoCheckLeavingToMenu
  if (global.coSocket < 0 && global.coServer < 0) exit;
  if (!instance_exists(objTransition)) {
    global.coLoggedTarget = undefined;
    exit;
  }
  var _target = objTransition.target;
  if (!variable_global_exists("coLoggedTarget") || global.coLoggedTarget != _target) {
    global.coLoggedTarget = _target;
    CoDebug("transition to " + string(_target));
  }
  if (!CoTargetUnloadsMods(_target)) exit;
  CoDebug("holding the transition, closing the session first");
  with (objTransition) instance_destroy(id);  // the mod interpreter needs the argument
  CoBanner("Closing the co-op session...");
  CoLeaveThenMenu(0);
  Trace("[CoopTest] leaving to the main menu: session closed");

#define CoCmdStop(_args)
  CoLeave(1);
  Trace("[CoopTest] stopped");

// Leave the session on purpose: tell the other side, then close everything.
#define CoLeave(_reason)
  CoDebug("leaving, reason=" + string(_reason) + " room=" + string(room));
  if (global.coSocket >= 0) {
    var _buf = buffer_create(2, buffer_grow, 1);
    buffer_write(_buf, buffer_u8, 16);
    buffer_write(_buf, buffer_u8, _reason);
    CoSendBuffer(_buf);
  }
  CoClose();

// The other side left (BYE with a reason) or the connection dropped (_saidBye = false).
#define CoPeerGone(_saidBye, _reason)
  if (global.coIsHost) {
    // keep listening so the player can join again
    if (global.coSocket >= 0) network_destroy(global.coSocket);
    global.coSocket = -1;
    global.coRemoteSeen = false;
    CoPuppetRemove();
    CoWorldReset();
    if (!_saidBye) CoBanner("Player 2 lost connection");
    else if (_reason == 2) CoBanner("Player 2 is reloading the world...");
    else CoBanner("Player 2 left the game");
    Trace("[CoopTest] player 2 gone, still hosting on port " + string(global.coPort));
  } else {
    CoClose();
    CoBanner(_saidBye ? "The host ended the session" : "Lost connection to the host");
    if (room != rmMainMenu) CoMenuLater();
  }

// ---------- islands ----------

// Host: notice a purchase; RELOAD goes out once the game has written the save.
// Never read global.ownedLands: wrapping a game data structure for the mod
// (variable_global_get(..., "grid")) makes the mod loader destroy the game's own grid when mods
// are unloaded, which later breaks the Buy land screen ("REAL argument is undefined").
// Instead: the Buy land screen (objPurchaseLand plots) pauses the game, so the only thing that
// can lower the player's coins there is buying a plot. Runs on GUI frames (works while paused).
#define CoLandPoll
  if (global.coLandPendingSince < 0) {
    if (instance_exists(objPurchaseLand) && instance_exists(objPlayer)) {
      var _coins = objPlayer.coins;
      if (global.coBuyLandCoins >= 0 && _coins < global.coBuyLandCoins) {
        global.coLandPendingSince = global.coGuiFrame;
        Trace("[CoopTest] island bought, waiting for the save to finish");
      }
      global.coBuyLandCoins = _coins;
    } else {
      global.coBuyLandCoins = -1;
    }
  }
  if (global.coLandPendingSince >= 0
      && global.coGuiFrame - global.coLandPendingSince >= global.coLandSaveWaitFrames) {
    CoSendReload();
  }

#define CoSendReload
  global.coLandPendingSince = -1;
  var _buf = buffer_create(1, buffer_grow, 1);
  buffer_write(_buf, buffer_u8, 17);
  CoSendBuffer(_buf);
  CoBanner("New island! Player 2 is reloading the world...");

#define OnSave
  CoDebug("OnSave room=" + string(room));

#define OnSaveEnd
  CoDebug("OnSaveEnd");
  if (global.coIsHost && global.coSocket >= 0 && global.coLandPendingSince >= 0) CoSendReload();

// Client: leave without saving, remember the host, reconnect after the save is loaded again.
#define CoReloadForIsland
  var _message = "The host bought an island: load your save to continue";
  global.coReconnectPending = true;
  CoLeave(2);
  CoBanner(_message);
  // survives the mod unload that happens on the way to the main menu (see Main)
  variable_global_set("coopCarryReconnect", true);
  variable_global_set("coopCarryIp", global.coLastIp);
  variable_global_set("coopCarryPort", global.coPort);
  if (room != rmMainMenu) CoMenuLater();

// Same transition the pause menu uses; no save, the client plays on a copy of the host's world.
#define CoGoToMainMenu
  if (!variable_global_exists("coopCarryReconnect")) {
    variable_global_set("coopCarryReconnect", false);
    variable_global_set("coopCarryIp", global.coLastIp);
    variable_global_set("coopCarryPort", global.coPort);
  }
  variable_global_set("coopCarryBanner", global.coBannerFrames > 0 ? global.coBannerText : "");
  var _transition = instance_create_depth(0, 0, -100000, objTransition);
  _transition.target = rmMainMenu;

#define CoPuppetRemove
  if (global.coPuppet != noone && instance_exists(global.coPuppet)) instance_destroy(global.coPuppet);
  global.coPuppet = noone;

// ---------- on-screen banner ----------

#define CoBanner(_text)
  global.coBannerText = _text;
  global.coBannerFrames = global.coBannerDuration;

// OnStep does not run while the game is paused (any open player menu), OnDrawGUI always does.
// Everything that must keep working during a pause lives here, on its own frame counter.
#define OnDrawGUI
  global.coGuiFrame += 1;
  CoTickAlways();
  CoDrawBanner();

#define CoTickAlways
  if (global.coBannerFrames > 0) global.coBannerFrames -= 1;
  if (global.coMenuAt >= 0 && global.coGuiFrame >= global.coMenuAt) {
    global.coMenuAt = -1;
    if (room != rmMainMenu) CoGoToMainMenu();
  }
  CoCheckLeavingToMenu();
  if (global.coSocket < 0) exit;
  if (global.coGuiFrame - global.coLastRecvGui > global.coSilenceTimeoutFrames) {
    Trace("[CoopTest] no data from the other side for 5 s");
    CoPeerGone(false, 0);
    exit;
  }
  if (global.coGuiFrame mod global.coKeepaliveFrames == 0) CoSend(5, 0, 0);
  if (global.coIsHost) CoLandPoll();

#define CoDrawBanner
  if (global.coBannerFrames <= 0) exit;
  var _alpha = min(1, global.coBannerFrames / global.coBannerFade);
  var _cx = display_get_gui_width() / 2;
  var _top = display_get_gui_height() * 0.12;
  var _padX = 24;
  var _padY = 12;
  var _w = string_width(global.coBannerText) + _padX * 2;
  var _h = string_height(global.coBannerText) + _padY * 2;
  draw_set_alpha(_alpha * 0.8);
  draw_set_color(c_black);
  draw_rectangle(_cx - _w / 2, _top, _cx + _w / 2, _top + _h, false);
  draw_set_alpha(_alpha);
  draw_set_color(c_white);
  draw_set_halign(fa_center);
  draw_set_valign(fa_middle);
  draw_text(_cx, _top + _h / 2, global.coBannerText);
  draw_set_halign(fa_left);
  draw_set_valign(fa_top);
  draw_set_alpha(1);

#define CoClose
  if (global.coSocket >= 0) network_destroy(global.coSocket);
  if (global.coLoopPeer >= 0) network_destroy(global.coLoopPeer);
  if (global.coServer >= 0) network_destroy(global.coServer);
  global.coSocket = -1;
  global.coLoopPeer = -1;
  global.coServer = -1;
  global.coLoopMode = false;
  global.coRemoteSeen = false;
  if (variable_global_exists("coBannerFrames")) CoPuppetRemove();  // absent in older builds
  CoWorldReset();

// ---------- events ----------

#define OnNetworkAsync(_map)
  var _type = ds_map_find_value(_map, "type");
  var _id = ds_map_find_value(_map, "id");
  switch (_type) {
    case network_type_connect:
    case network_type_non_blocking_connect:
      if (global.coServer >= 0 && _id == global.coServer) {
        if (!global.coIsHost) {
          // loop mode: this is our own client arriving at our own server
          global.coLoopPeer = ds_map_find_value(_map, "socket");
          Trace("[CoopTest] loop: server accepted own client");
          exit;
        }
        if (global.coSocket >= 0) {
          Trace("[CoopTest] extra player rejected");
          network_destroy(ds_map_find_value(_map, "socket"));
          exit;
        }
        global.coSocket = ds_map_find_value(_map, "socket");
        global.coLastRecvGui = global.coGuiFrame;
        Trace("[CoopTest] player connected");
        CoSend(1, 1, 0);
      }
      break;
    case network_type_disconnect:
      if (global.coSocket >= 0 && ds_map_find_value(_map, "socket") == global.coSocket) {
        Trace("[CoopTest] other player disconnected");
        CoPeerGone(false, 0);
      }
      break;
    case network_type_data:
      if (_id == global.coSocket) {
        global.coLastRecvGui = global.coGuiFrame;
        CoHandlePacket(ds_map_find_value(_map, "buffer"));
      }
      else if (_id == global.coLoopPeer) CoHandleLoopPacket(ds_map_find_value(_map, "buffer"));
      break;
  }

#define OnRoomLoad
  // the session is closed before going to the main menu (CoCheckLeavingToMenu), never here
  if (room == rmGame && global.coReconnectPending && global.coSocket < 0) {
    global.coReconnectAt = global.coFrame + global.coReconnectDelayFrames;
    CoBanner("Reconnecting to the host...");
  }
  CoWorldRoomLoad();

#define OnStep
  global.coFrame += 1;
  if (global.coReconnectAt >= 0 && global.coFrame >= global.coReconnectAt) {
    global.coReconnectAt = -1;
    global.coReconnectPending = false;
    if (global.coSocket < 0 && global.coServer < 0) CoJoin(global.coLastIp, global.coPort);
  }
  if (global.coSocket < 0) exit;
  CoWorldStep();
  if (!global.coIsHost && !global.coHelloSeen
      && global.coFrame - global.coConnectFrame == global.coHelloTimeoutFrames) {
    Trace("[CoopTest] connected, but the host does not answer. The port may be held by a stale"
      + " socket from an older build: restart the host game or use another port (/cohost 7778)");
  }
  if (!global.coIsHost && global.coFrame mod 60 == 0) {
    global.coPingsSent += 1;
    CoSend(2, global.coFrame, 0);
  }
  if (global.coFrame mod 4 == 0 && instance_exists(objPlayer)) {
    CoSend(4, objPlayer.x, objPlayer.y);
  }
  var _puppetAlive = global.coPuppet != noone && instance_exists(global.coPuppet);
  if (global.coRemoteSeen && !_puppetAlive) {
    global.coPuppet = ModObjectSpawn(global.coRemoteX, global.coRemoteY, -10000);
    global.coPuppet.antiCull = true;
    InstanceAssignMethod(global.coPuppet, "draw", ScriptWrap(CoPuppetDraw), false);
    _puppetAlive = true;
  }
  if (_puppetAlive) {
    global.coPuppet.x = global.coRemoteX;
    global.coPuppet.y = global.coRemoteY;
    global.coPuppet.visible = global.coRemoteSeen;
  }

#define CoPuppetDraw
  draw_sprite_ext(sprPlayerIdle, 0, x, y, 1, 1, 0, c_aqua, 0.8);
  // small centered label right above the sprite's top edge
  draw_set_halign(fa_center);
  draw_set_valign(fa_bottom);
  draw_text_transformed(x, y - sprite_get_yoffset(sprPlayerIdle) - 2, "P2", 0.5, 0.5, 0);
  draw_set_halign(fa_left);
  draw_set_valign(fa_top);
