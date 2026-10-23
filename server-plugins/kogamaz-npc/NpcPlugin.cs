using System.Numerics;
using System.Text.Json;
using System.Text.RegularExpressions;
using OpenKogama.Api;

namespace KogamazNpc;

// NPC-боты KoGaMaZ.
//
// Как это работает без правки клиента:
// 1. В меню строительства есть предмет «NPC» - маленький Text Message с
//    текстом $$npc, ручка, за которую NPC двигают и удаляют. Сервер прячет
//    такие маркеры в режиме игры.
// 2. На каждом маркере стоит живой NPC - аватар с ником и оружием, как у
//    игрока. В строительстве он только стоит (превью), в игре и в
//    тест-игре строительства - воюет. Бот с командой, которая есть на
//    карте, числится в ней (сервер KoGaMaZ, server-patches/): свои его не
//    ранят, и он их не трогает.
// 3. В режиме строительства плагин записывает найденные маркеры в
//    plugins/data/kogamaz-npc-markers.json - по нему лаунчер показывает
//    список NPC карты. Только что поставленный NPC попадает ещё и в
//    kogamaz-npc-open.json: лаунчер сам открывает на нём окно настроек.
// 4. Лаунчер пишет настройки в plugins/data/kogamaz-npc-config.json, плагин
//    подхватывает их за секунду.
// Ранняя версия ставила вместо маркера копию класса аватара с меткой
// kogamazNpc - такие NPC тоже узнаются, в игре класс убирается.
public sealed partial class NpcPlugin : IPlugin
{
    const string MarkersName = "kogamaz-npc-markers.json";
    const string ConfigName = "kogamaz-npc-config.json";
    const string OpenName = "kogamaz-npc-open.json";
    const string AvatarsName = "kogamaz-npc-avatars";
    const long ReopenEveryMs = 2000;
    const long SettingsHoldMs = 700;   // запасное правило: столько держали маркер, не двигая
    const float MarkerMoved = 0.05f;
    const float SpawnLift = 0.3f;
    const long ThinkEveryMs = 100;
    const long RetargetEveryMs = 500;
    const float EyeHeight = 1.5f;
    const float WanderRadius = 8f;
    const long WanderGiveUpMs = 8000;
    const long StuckCheckMs = 1000;
    const float StuckDistance = 0.3f;
    const float StuckJump = 0.4f;
    const float ArriveDistance = 0.8f;
    const float PositionMatch = 0.75f;
    const long HuntMs = 6000;
    const long RevengeMs = 3000;
    const long WeaponSwapMs = 1500;
    const long ApproachReplanMs = 250;
    const long DodgeWindowMs = 400;
    const float LowHealth = 0.4f;
    const float RunSpeed = 8f;
    const float WanderJumpChance = 0.008f;   // за шаг ИИ (100 мс) - раз в ~12 с
    // «безумный»
    const long CunningHuntMs = 12000;
    const long AmbushMs = 4000;
    const long PeekLeadMs = 150;              // выглядывает чуть раньше перезарядки
    const long BhopEveryMs = 350;
    const float FinishDamage = 60f;           // столько нанёс цели - добивает, не прячась
    const float CoverHealth = 0.5f;
    const float CoverCloseDistance = 6f;      // ближе цель - прятаться поздно
    const int CoverDirections = 12;
    static readonly float[] CoverRadii = [3f, 6f];
    static readonly Vector3 Waist = Vector3.UnitY * 0.8f;
    // «ультра безумие»
    const long StalkReplanMs = 1000;
    const long StalkWaitMs = 3000;            // засада за углом, когда цель бежит на бота
    const long StalkWaitPauseMs = 4000;
    const float StalkStep = 6f;               // идёт к точке короткими отрезками
    const float ClosingSpeed = 2f;
    const int PeekDirections = 16;
    const long GlitchSearchMs = 800;
    const float GlitchMinDistance = 6f;
    const float GlitchWallReach = 2f;         // стенка - вплотную, не дальше
    const float ChestShare = 0.6f;            // грудь - на такой доле высоты глаз
    static readonly float[] GlitchRadii = [0f, 1.5f, 3f, 5f, 7f];
    const float StalkFar = 40f;               // дальше не выглядывает: цель за 40 м - уже не видно, кто где
    // «общается»
    const float HearDistance = 12f;           // слышит игрока, который пишет рядом
    const float ChatKeepDistance = 20f;       // дальше - разговор окончен
    const long ChatHoldMs = 20000;            // столько молчат - разговор окончен
    const float GreetDistance = 6f;
    const long GreetCheckMs = 500;
    const float TalkFar = 4f;                 // собеседник дальше - подходит
    const float TalkNear = 2.5f;

    static readonly Dictionary<string, Team> TeamSpawns = new(StringComparer.OrdinalIgnoreCase)
    {
        ["SpawnPointBlue"] = Team.Blue,
        ["SpawnPointRed"] = Team.Red,
        ["SpawnPointGreen"] = Team.Green,
        ["SpawnPointYellow"] = Team.Yellow,
    };

    sealed class Bot(IWorld world, int marker, Vector3 home, NpcSettings settings, string name, Team? team, bool joined)
    {
        public IWorld World => world;
        public int Marker => marker;
        public Vector3 Home { get; set; } = home;
        public Quaternion Facing { get; set; }
        // Превью: стоит на маркере с оружием в руке. Воюет бот в игре и в
        // тест-игре строительства (когда хоть кто-то вышел из строительства).
        public bool Preview { get; set; }
        public NpcSettings Settings => settings;
        public string Name => name;
        public Difficulty Level { get; } = Difficulties.Of(settings.Difficulty);
        public Team? Team => team;
        // Бот числится в команде карты: у него цвет команды, свои его не ранят.
        // Нет такой команды на карте - команда только в нике ([RED] Vasya).
        public bool Joined => joined;
        public bool Talks => string.Equals(settings.Mode, "talk", StringComparison.OrdinalIgnoreCase);
        // манекен: стоит, ничего не делает, его убивают - возрождается
        public bool Idle => string.Equals(settings.Mode, "idle", StringComparison.OrdinalIgnoreCase);
        public bool Attacks => !Talks && !Idle && !string.Equals(settings.Mode, "walk", StringComparison.OrdinalIgnoreCase);
        public INpc Npc { get; set; } = null!;
        public float BaseSpeed { get; set; }
        public Weapon? Weapon { get; set; }
        public long NextSwitch { get; set; }
        public long NextThink { get; set; }
        public long NextRetarget { get; set; }
        public object? Target { get; set; }
        public long TargetSince { get; set; }
        public long NextShot { get; set; }
        public long? DeadAt { get; set; }
        public Vector3? WanderGoal { get; set; }
        public long WanderUntil { get; set; }
        public long PauseUntil { get; set; }
        public Vector3 LastCheck { get; set; }
        public long LastCheckAt { get; set; }
        // бой: манёвры, слежка за целью, попадания по самому боту
        public long NextManeuver { get; set; }
        public long NextWeaponSwap { get; set; }
        public int StrafeSide { get; set; } = 1;
        public int FlankSide { get; set; } = 1;
        public bool TargetVisible { get; set; }
        public Vector3? LastSeen { get; set; }
        public long LastSeenAt { get; set; }
        public object? Tracked { get; set; }
        public Vector3 TrackPos { get; set; }
        public long TrackAt { get; set; }
        public Vector3 TargetVelocity { get; set; }
        public object? HurtBy { get; set; }
        public long HurtAt { get; set; } = long.MinValue / 2;
        public bool Dodged { get; set; }
        // «безумный»: укрытие между выстрелами, засада, добивание, банни-хоп
        public Vector3? CoverGoal { get; set; }
        public long CoverUntil { get; set; }
        public Vector3 PeekFrom { get; set; }
        public bool Ambush { get; set; }
        public long AmbushUntil { get; set; }
        public Vector3? HuntGoal { get; set; }
        public float DealtToTarget { get; set; }
        public int Threats { get; set; }
        public bool Packed { get; set; }
        public long NextBhop { get; set; }
        public long NextRailDodge { get; set; }
        // «ультра»: подход к цели и стрельба из-за стенки
        public Vector3? Approach { get; set; }
        public long NextApproach { get; set; }
        public long WaitUntil { get; set; }
        public long NextWait { get; set; }
        public Vector3? Glitch { get; set; }
        public long NextGlitchSearch { get; set; }
        // «общается»: собеседник (actor игрока) и реплика, которую «печатает»
        public Chatter? Chat { get; set; }
        public int Partner { get; set; }
        public long PartnerAt { get; set; } = long.MinValue / 2;
        public string? Pending { get; set; }
        public long PendingAt { get; set; }
        public long NextGreetCheck { get; set; }
        public string? KilledBy { get; set; }
    }

    readonly object _sync = new();
    readonly List<Bot> _bots = [];
    IServer _server = null!;
    string _markersPath = "";
    string _configPath = "";
    string _openPath = "";
    string _avatarsPath = "";
    ConfigFile _config = new();
    DateTime _configStamp;
    string _lastMarkers = "";

    public void Load(IServer server)
    {
        _server = server;
        // Сервер ищет плагины в plugins/ своей рабочей папки, там же и data/.
        string data = Path.GetFullPath(Path.Combine("plugins", "data"));
        _markersPath = Path.Combine(data, MarkersName);
        _configPath = Path.Combine(data, ConfigName);
        _openPath = Path.Combine(data, OpenName);
        _avatarsPath = Path.Combine(data, AvatarsName);
        ReloadConfig(force: true);

        server.Tick += OnTick;
        server.Update += OnUpdate;
        server.Chat += OnChat;
        server.NpcKilled += OnNpcKilled;
        server.NpcDamaged += OnNpcDamaged;
        server.ObjectAdded += OnObjectAdded;
        server.ObjectLocked += OnObjectLocked;
        server.ObjectDataChanged += OnObjectDataChanged;
        server.Log("kogamaz-npc: ready");
    }

    public void Unload()
    {
        lock (_sync)
        {
            foreach (Bot bot in _bots.Where(bot => bot.Npc.Exists)) bot.Npc.Remove();
            _bots.Clear();
        }
    }

    // --- маркеры -------------------------------------------------------------

    // Figure - NPC ранней версии (класс аватара с меткой NpcTag): в игре его
    // убираем, иначе игрокам предлагался бы лишний класс. ClassTeam -
    // команда, выбранная в окне класса. Текст $$npc сервер прячет сам.
    sealed record Marker(int Id, Vector3 Position, Quaternion Rotation, string Label, bool Figure, Team? ClassTeam);

    const string NpcTag = "kogamazNpc";   // то же имя пишет add-npc-item.py
    const string ClassType = "AvatarSpawnRoleCreator";
    const string ClassTeamKey = "team";

    [GeneratedRegex(@"^\$\$npc(?:[_-]([A-Za-z0-9_-]+))?$", RegexOptions.IgnoreCase)]
    private static partial Regex MarkerText();

    [GeneratedRegex("<[^>]*>")]
    private static partial Regex Tags();

    // known - id NPC-классов этой карты, которые плагин уже видел с меткой:
    // если клиент при правке данных класса метку потеряет, NPC не пропадёт.
    static Marker? MarkerOf(IWorldObject obj, IReadOnlySet<int> known)
    {
        if (obj.Data.ContainsKey(NpcTag) || known.Contains(obj.Id) && obj.Type == ClassType)
        {
            Team? team = obj.Data.TryGetValue(ClassTeamKey, out object? raw) && raw is int value && Enum.IsDefined((Team)value)
                ? (Team)value
                : null;
            return new Marker(obj.Id, obj.Position, obj.Rotation, "", Figure: true, team);
        }
        if (!string.Equals(obj.Type, "TextMsg", StringComparison.OrdinalIgnoreCase)) return null;
        if (!obj.Data.TryGetValue("text", out object? value2) || value2?.ToString() is not string text) return null;
        Match match = MarkerText().Match(Tags().Replace(text, "").Trim());
        return match.Success ? new Marker(obj.Id, obj.Position, obj.Rotation, match.Groups[1].Value, Figure: false, null) : null;
    }

    // Убранные в игре фигуры: объекта в мире уже нет, а бот должен остаться.
    readonly Dictionary<IWorld, Dictionary<int, Marker>> _removed = [];
    readonly HashSet<IWorld> _cleaned = [];

    HashSet<int> KnownFigures(IWorld world) =>
        Json.Read<MarkersFile>(_markersPath).Worlds.TryGetValue(world.Id.ToString(), out WorldMarkers? known)
            ? [.. known.Markers.Where(info => info.Figure).Select(info => info.Id)]
            : [];

    List<Marker> Markers(IWorld world)
    {
        HashSet<int> known = KnownFigures(world);
        var markers = world.Objects.Select(obj => MarkerOf(obj, known)).OfType<Marker>().ToList();
        if (_removed.TryGetValue(world, out var removed))
            markers.AddRange(removed.Values.Where(gone => markers.All(marker => marker.Id != gone.Id)));
        return [.. markers.OrderBy(marker => marker.Id)];
    }

    // --- раз в секунду -----------------------------------------------------

    void OnTick()
    {
        try
        {
            lock (_sync)
            {
                ReloadConfig(force: false);
                SaveEditMarkers();
                Reconcile(Environment.TickCount64);
            }
        }
        catch (Exception error)
        {
            Console.WriteLine($"kogamaz-npc: tick failed: {error}");
        }
    }

    void ReloadConfig(bool force)
    {
        DateTime stamp = File.Exists(_configPath) ? File.GetLastWriteTimeUtc(_configPath) : DateTime.MinValue;
        if (!force && stamp == _configStamp) return;
        _configStamp = stamp;
        _config = Json.Read<ConfigFile>(_configPath);
    }

    // В режиме строительства запоминаем, где стоят NPC, - для лаунчера.
    void SaveEditMarkers()
    {
        List<IWorld> editing = [.. _server.Worlds.Where(world => !world.Play)];
        if (editing.Count == 0) return;

        MarkersFile file = Json.Read<MarkersFile>(_markersPath);
        foreach (IWorld world in editing)
        {
            List<Marker> markers = Markers(world);
            // метку, потерянную при правке класса, возвращаем
            foreach (Marker marker in markers.Where(marker => marker.Figure))
                if (world.Find(marker.Id) is { } obj && !obj.Data.ContainsKey(NpcTag))
                    world.SetData(marker.Id, NpcTag, 1);

            file.Worlds[world.Id.ToString()] = new WorldMarkers
            {
                Title = world.Name,
                Markers = [.. markers.Select(marker => new MarkerInfo
                {
                    Id = marker.Id,
                    X = marker.Position.X,
                    Y = marker.Position.Y,
                    Z = marker.Position.Z,
                    Name = marker.Label.Length > 0 ? marker.Label : null,
                    Figure = marker.Figure,
                    Team = marker.ClassTeam?.ToString().ToLowerInvariant(),
                })],
            };
        }

        string json = JsonSerializer.Serialize(file, Json.Options);
        if (json == _lastMarkers) return;
        _lastMarkers = json;
        Json.Write(_markersPath, file);
    }

    NpcSettings SettingsFor(IWorld world, Marker marker, int index)
    {
        NpcSettings? found = null;
        if (_config.Worlds.TryGetValue(world.Id.ToString(), out WorldSettings? settings))
        {
            settings.Npcs.TryGetValue(marker.Id.ToString(), out found);
            // В игре id объектов могут не совпасть с режимом строительства -
            // тогда узнаём маркер по месту, где он стоял при настройке.
            if (found is null && Json.Read<MarkersFile>(_markersPath).Worlds.TryGetValue(world.Id.ToString(), out WorldMarkers? known))
            {
                MarkerInfo? near = known.Markers.MinBy(info => Vector3.Distance(new Vector3(info.X, info.Y, info.Z), marker.Position));
                if (near is not null && Vector3.Distance(new Vector3(near.X, near.Y, near.Z), marker.Position) <= PositionMatch)
                    settings.Npcs.TryGetValue(near.Id.ToString(), out found);
            }
        }
        found ??= new NpcSettings();
        if (string.IsNullOrWhiteSpace(found.Name)) found.Name = marker.Label.Length > 0 ? marker.Label : $"Bot {index + 1}";
        return found;
    }

    // Приводим ботов к маркерам и настройкам: в игре - бойцы, в
    // строительстве - превью на месте маркера.
    void Reconcile(long now)
    {
        List<IWorld> open = [.. _server.Worlds];
        List<IWorld> playing = [.. open.Where(world => world.Play)];

        // миры, которые закрылись, - их боты ушли вместе с сессией
        foreach (Bot bot in _bots.Where(bot => !open.Contains(bot.World)).ToList())
        {
            if (bot.Npc.Exists) bot.Npc.Remove();
            _bots.Remove(bot);
        }
        foreach (IWorld gone in _removed.Keys.Where(world => !playing.Contains(world)).ToList())
            _removed.Remove(gone);
        _cleaned.RemoveWhere(world => !playing.Contains(world));

        foreach (IWorld world in open)
        {
            List<Marker> markers = Markers(world);
            var wanted = new Dictionary<int, (Marker Marker, NpcSettings Settings)>();
            for (int i = 0; i < markers.Count; i++)
                wanted[markers[i].Id] = (markers[i], SettingsFor(world, markers[i], i));

            List<Team> teams = MapTeams(world);
            _teams[world] = teams;
            bool preview = !Fighting(world);

            foreach (Bot bot in _bots.Where(bot => bot.World == world).ToList())
            {
                // команды карты поменялись (поставили или убрали точку
                // появления) - бот вступает в команду или выходит из неё
                if (wanted.TryGetValue(bot.Marker, out var want) && want.Settings.Key() == bot.Settings.Key() && bot.Npc.Exists
                    && Joins(teams, TeamOf(want.Settings, want.Marker)) == bot.Joined)
                {
                    if (preview && !bot.Preview) ToPreview(bot);
                    bot.Preview = preview;
                    if (bot.Preview) Follow(bot, want.Marker);
                    continue;
                }
                if (bot.Npc.Exists) bot.Npc.Remove();
                _bots.Remove(bot);
            }

            foreach ((int id, var want) in wanted)
            {
                if (_bots.Any(bot => bot.World == world && bot.Marker == id)) continue;
                Spawn(world, want.Marker, want.Settings, now, preview);
            }

            if (world.Play) RemoveFigures(world, markers);
        }
        foreach (IWorld gone in _teams.Keys.Where(world => !open.Contains(world)).ToList())
            _teams.Remove(gone);
    }

    // В игре бот воюет всегда, в строительстве - пока кто-то в тест-игре.
    static bool Fighting(IWorld world) => world.Play || world.Players.Any(player => !player.Building);

    // Команды карты - как их видит сервер: точки появления цветов и команды
    // классов (у AvatarSpawnRoleCreator ключ team, 5 - без команды).
    static List<Team> MapTeams(IWorld world) =>
    [
        .. world.Objects
            .Select(obj => TeamSpawns.TryGetValue(obj.Type, out Team spawn) ? spawn
                : obj.Type == ClassType && obj.Data.TryGetValue(ClassTeamKey, out object? raw) && raw is int value && Enum.IsDefined((Team)value) ? (Team)value
                : (Team?)null)
            .OfType<Team>()
            .Distinct(),
    ];

    // Команд на карте меньше двух - все игроки в одной, своих и чужих нет.
    static bool Joins(List<Team> teams, Team? team) => team is Team t && teams.Count > 1 && teams.Contains(t);

    readonly Dictionary<IWorld, List<Team>> _teams = [];

    // Все вернулись в строительство: бот снова на маркере и здоров.
    static void ToPreview(Bot bot)
    {
        bot.Target = null;
        bot.WanderGoal = null;
        bot.PauseUntil = 0;
        bot.LastSeen = null;
        bot.CoverGoal = null;
        bot.HuntGoal = null;
        bot.AmbushUntil = 0;
        bot.Approach = null;
        bot.Glitch = null;
        bot.WaitUntil = 0;
        bot.Pending = null;
        bot.PartnerAt = long.MinValue / 2;
        bot.Npc.LookYaw = null;
        if (!bot.Npc.Alive) return;   // мёртвого вернёт возрождение
        bot.Npc.Respawn(bot.Home);
        bot.Npc.Face(YawOf(bot.Facing));
    }

    // Маркер в строительстве подвинули или повернули - превью за ним.
    static void Follow(Bot bot, Marker marker)
    {
        Vector3 home = marker.Position + Vector3.UnitY * SpawnLift;
        if (Vector3.Distance(home, bot.Home) < MarkerMoved && marker.Rotation == bot.Facing) return;
        bot.Home = home;
        bot.Facing = marker.Rotation;
        bot.Npc.Teleport(home, YawOf(marker.Rotation));
    }

    // Только что поставленный NPC - лаунчер откроет на нём окно настроек.
    void OnObjectAdded(IWorld world, IWorldObject obj, IPlayer? by)
    {
        if (world.Play || by is null || MarkerOf(obj, new HashSet<int>()) is null) return;
        lock (_sync) RequestOpen(world, obj.Id);
    }

    readonly Dictionary<(int World, int Marker), long> _opened = [];

    void RequestOpen(IWorld world, int marker)
    {
        long now = Environment.TickCount64;
        if (_opened.TryGetValue((world.Id, marker), out long last) && now - last < ReopenEveryMs) return;
        _opened[(world.Id, marker)] = now;
        try
        {
            Json.Write(_openPath, new OpenRequest { World = world.Id, Title = world.Name, Marker = marker, At = DateTime.UtcNow });
        }
        catch (Exception error)
        {
            Console.WriteLine($"kogamaz-npc: open request failed: {error.Message}");
        }
    }

    // ПКМ по кубу NPC -> Settings: окно настроек Text Message рисует клиент,
    // свои поля туда не добавить. Зато сервер видит, что маркер захватили на
    // правку и записали его данные (OK в окне), - тогда лаунчер открывает
    // окно NPC. Запасной признак: маркер держали, не двигая, и отпустили.
    readonly Dictionary<(int World, int Marker), (long At, Vector3 Position)> _held = [];

    static bool IsTextMarker(IWorldObject? obj) => obj is not null && MarkerOf(obj, new HashSet<int>()) is { Figure: false };

    void OnObjectLocked(IWorld world, int id, IPlayer? by, bool locked)
    {
        if (world.Play || by is null) return;
        lock (_sync)
        {
            IWorldObject? obj = world.Find(id);
            if (!IsTextMarker(obj)) return;
            long now = Environment.TickCount64;
            if (locked)
            {
                _held[(world.Id, id)] = (now, obj!.Position);
                _server.Log($"kogamaz-npc: marker {id} lock by {by.Name}");
                return;
            }
            if (!_held.Remove((world.Id, id), out var held)) return;
            float moved = Vector3.Distance(held.Position, obj!.Position);
            long ms = now - held.At;
            _server.Log($"kogamaz-npc: marker {id} unlock by {by.Name} (held {ms} ms, moved {moved:0.00} m)");
            if (moved < MarkerMoved && ms >= SettingsHoldMs) RequestOpen(world, id);
        }
    }

    void OnObjectDataChanged(IWorld world, IWorldObject obj, IPlayer? by)
    {
        if (world.Play || by is null || !IsTextMarker(obj)) return;
        lock (_sync)
        {
            _server.Log($"kogamaz-npc: marker {obj.Id} data by {by.Name}");
            RequestOpen(world, obj.Id);
        }
    }

    // Облик из .kgavatar, который лаунчер положил в data/kogamaz-npc-avatars.
    readonly Dictionary<string, (DateTime Stamp, AvatarSkin? Skin)> _skins = new(StringComparer.OrdinalIgnoreCase);

    AvatarSkin? SkinOf(string? file)
    {
        if (string.IsNullOrWhiteSpace(file)) return null;
        string name = Path.GetFileName(file);   // только файл из папки обликов
        string path = Path.Combine(_avatarsPath, name);
        if (!File.Exists(path))
        {
            _server.Log($"kogamaz-npc: avatar {name} not found");
            return null;
        }
        DateTime stamp = File.GetLastWriteTimeUtc(path);
        if (_skins.TryGetValue(name, out var cached) && cached.Stamp == stamp) return cached.Skin;

        AvatarSkin? skin = null;
        try
        {
            AvatarSkin read = AvatarSkin.FromJson(File.ReadAllText(path));
            if (read.Parts is { Count: > 0 } && read.Parts.All(part => !string.IsNullOrEmpty(part.Bone) && Base64(part.Cubes)))
                skin = read with { Accessories = read.Accessories ?? [] };
        }
        catch (Exception error) when (error is JsonException or InvalidDataException or IOException or NotSupportedException)
        {
        }
        if (skin is null) _server.Log($"kogamaz-npc: bad avatar {name}");
        _skins[name] = (stamp, skin);
        return skin;
    }

    static bool Base64(string? text) =>
        !string.IsNullOrEmpty(text) && Convert.TryFromBase64String(text, new byte[text.Length], out _);

    // NPC-класс в игре не нужен: убираем, пока мир ещё не ушёл игроку.
    void RemoveFigures(IWorld world, List<Marker> markers)
    {
        foreach (Marker figure in markers.Where(marker => marker.Figure && world.Find(marker.Id) is not null))
        {
            if (!_removed.TryGetValue(world, out var removed)) _removed[world] = removed = [];
            removed[figure.Id] = figure;
            world.Remove(figure.Id);
        }
    }

    // «class» - команда из окна класса (ПКМ в строительстве).
    static Team? TeamOf(NpcSettings settings, Marker marker) =>
        string.Equals(settings.Team, "class", StringComparison.OrdinalIgnoreCase) ? marker.ClassTeam : Teams.Of(settings.Team);

    void Spawn(IWorld world, Marker marker, NpcSettings settings, long now, bool preview)
    {
        Team? team = TeamOf(settings, marker);
        bool joined = Joins(_teams.GetValueOrDefault(world) ?? [], team);
        // в команде карты цвет виден и так, иначе - тег в нике
        string name = team is Team t && !joined ? $"{Teams.Tag(t)} {settings.Name}" : settings.Name!;
        var bot = new Bot(world, marker.Id, marker.Position + Vector3.UnitY * SpawnLift, settings, name, team, joined)
        {
            Facing = marker.Rotation,
            Preview = preview,
        };
        AvatarSkin? skin = SkinOf(settings.Avatar);
        try
        {
            bot.Npc = world.SpawnNpc(name, bot.Home, YawOf(marker.Rotation), skin);
        }
        catch (Exception error) when (skin is not null)
        {
            // облик не встал - пусть будет обычный бот, а не никакого
            _server.Log($"kogamaz-npc: avatar {settings.Avatar} failed: {error.Message}");
            bot.Npc = world.SpawnNpc(name, bot.Home, YawOf(marker.Rotation));
        }
        if (joined) bot.Npc.Team = team;
        bot.BaseSpeed = bot.Npc.Speed;
        Equip(bot, now);
        // здоровье - как у игрока (100), его не трогаем
        bot.Npc.Speed = bot.BaseSpeed * bot.Level.SpeedScale;
        bot.LastCheck = bot.Home;
        bot.LastCheckAt = now;
        if (bot.Talks)
        {
            // память о собеседниках переживает смену настроек бота
            if (!_chatters.TryGetValue((world.Id, marker.Id), out Chatter? chat)) _chatters[(world.Id, marker.Id)] = chat = new Chatter(settings.Name!);
            chat.Name = settings.Name!;
            bot.Chat = chat;
        }
        _bots.Add(bot);
    }

    readonly Dictionary<(int World, int Marker), Chatter> _chatters = [];

    void Equip(Bot bot, long now)
    {
        // В режиме «ходит» оружие только для вида. Манекен и
        // «общается» держат его, только если выбрали конкретное.
        bot.Weapon = (bot.Talks || bot.Idle) && Weapons.IsAll(bot.Settings.Weapon) ? null : Weapons.Pick(bot.Settings.Weapon);
        bot.Npc.Holding = bot.Weapon?.Item;
        bot.NextSwitch = now + Weapons.SwitchEveryMs;
    }

    static float YawOf(Quaternion rotation)
    {
        Vector3 forward = Vector3.Transform(Vector3.UnitZ, rotation);
        return MathF.Atan2(forward.X, forward.Z) * 180f / MathF.PI;
    }

    void OnNpcKilled(INpc npc, IPlayer? by)
    {
        lock (_sync)
        {
            Bot? bot = _bots.FirstOrDefault(bot => bot.Npc.Id == npc.Id);
            if (bot is null) return;
            bot.DeadAt = Environment.TickCount64;
            bot.Target = null;
            bot.Pending = null;   // мёртвые не болтают
            if (bot.Chat is not null && by is not null)
            {
                bot.Chat.Killed(by.Name, bot.DeadAt.Value);
                bot.KilledBy = by.Name;
            }
        }
    }

    // --- ИИ ---------------------------------------------------------------

    void OnUpdate(float seconds)
    {
        try
        {
            lock (_sync)
            {
                // Новая игровая сессия: NPC-классы убираем сразу, а не на
                // секундном тике - мир уходит игроку меньше чем за секунду.
                foreach (IWorld world in _server.Worlds.Where(world => world.Play && !_cleaned.Contains(world)).ToList())
                {
                    RemoveFigures(world, Markers(world));
                    _cleaned.Add(world);
                }

                long now = Environment.TickCount64;
                foreach (Bot bot in _bots.ToList())
                {
                    if (!bot.Npc.Exists || now < bot.NextThink) continue;
                    bot.NextThink = now + ThinkEveryMs;
                    Think(bot, now);
                }
            }
        }
        catch (Exception error)
        {
            Console.WriteLine($"kogamaz-npc: update failed: {error}");
        }
    }

    void Think(Bot bot, long now)
    {
        INpc npc = bot.Npc;
        if (!npc.Alive)
        {
            bot.DeadAt ??= now;
            if (now - bot.DeadAt >= (long)(Math.Max(0f, bot.Settings.Respawn) * 1000f))
            {
                npc.Respawn(bot.Home);
                npc.Face(YawOf(bot.Facing));   // встаёт лицом туда же, куда смотрит маркер
                bot.DeadAt = null;
                bot.Target = null;
                bot.WanderGoal = null;
                bot.LastSeen = null;
                bot.CoverGoal = null;
                bot.HuntGoal = null;
                bot.AmbushUntil = 0;
                bot.Approach = null;
                bot.Glitch = null;
                bot.WaitUntil = 0;
                npc.LookYaw = null;
                if (Weapons.IsAll(bot.Settings.Weapon)) Equip(bot, now);
                if (bot.Chat is not null && bot.KilledBy is string killer && !bot.Preview)
                    Queue(bot, bot.Chat.Respawned(killer, now), now, quick: true);
                bot.KilledBy = null;
            }
            return;
        }

        // превью в строительстве не ходит и не стреляет, только показывает
        // оружие - при «все оружия» они сменяют друг друга
        if (bot.Preview)
        {
            if (npc.Walking) npc.Stop();
            if (Weapons.IsAll(bot.Settings.Weapon) && now >= bot.NextSwitch) Equip(bot, now);
            return;
        }

        if (bot.Idle)
        {
            if (npc.Walking) npc.Stop();
            npc.LookYaw = null;
            return;
        }

        if (bot.Talks)
        {
            Converse(bot, now);
            return;
        }

        // тактик меняет оружие сам, под дистанцию (в Fight)
        if (bot.Attacks && bot.Weapon is not null && Weapons.IsAll(bot.Settings.Weapon) && !bot.Level.Tactics && now >= bot.NextSwitch)
            Equip(bot, now);

        if (!bot.Attacks || bot.Weapon is null)
        {
            Wander(bot, now);
            return;
        }

        if (now >= bot.NextRetarget)
        {
            bot.NextRetarget = now + RetargetEveryMs;
            object? target = FindTarget(bot, now);
            // сложный и кошмар не бросают спрятавшуюся цель: идут туда,
            // где видели её последний раз («безумный» ищет дольше)
            if (target is null && bot.Level.Retaliates && bot.Target is not null && TargetEye(bot.Target) is not null
                && now - bot.LastSeenAt < (bot.Level.Cunning ? CunningHuntMs : HuntMs))
                target = bot.Target;
            if (bot.Level.Cunning) bot.Threats = CountThreats(bot);
            if (!ReferenceEquals(target, bot.Target))
            {
                bot.Target = target;
                bot.TargetSince = now;
            }
        }

        if (TargetEye(bot.Target) is not Vector3 eye)
        {
            bot.Target = null;
            bot.Tracked = null;   // та же цель после возрождения - заново
            bot.CoverGoal = null;
            bot.Approach = null;
            bot.Glitch = null;
            Wander(bot, now);
            return;
        }

        Fight(bot, eye, now);
    }

    // свои среди игроков есть, только если на карте несколько команд
    bool TeamsInWorld(IWorld world) => (_teams.GetValueOrDefault(world)?.Count ?? 0) > 1;

    // Противник ли: строящих игроков не трогает (в строительстве воюют
    // только с тест-игроками), своих по команде - тоже.
    bool Enemy(Bot bot, object other) => other switch
    {
        IPlayer player => player.Alive && !player.Building
            && !(bot.Team is Team team && TeamsInWorld(bot.World) && player.Team == team),
        Bot them => them != bot && them.World == bot.World && them.Npc.Alive
            && !(bot.Team is not null && bot.Team == them.Team)
            && !(bot.Team is null && them.Team is null && !them.Attacks),
        _ => false,
    };

    // Цель: ближайший видимый противник - игрок или чужой бот. Кошмар
    // предпочитает того, кого уже бьют свои, и того, кто попал в него.
    // «Ультра» знает всех на карте: никого не видно - берёт ближайшего.
    object? FindTarget(Bot bot, long now)
    {
        Vector3 from = bot.Npc.EyePosition;
        bool anywhere = bot.Level.Omniscient;
        var candidates = new List<(object Target, Vector3 Eye, float Score)>();

        void Consider(object target, Vector3 eye)
        {
            float distance = Vector3.Distance(from, eye);
            if (distance > bot.Level.Sight && !anywhere) return;
            float score = distance;
            if (bot.Level.Tactics)
            {
                if (bot.Team is not null && _bots.Any(mate => mate != bot && mate.Team == bot.Team && ReferenceEquals(mate.Target, target)))
                    score *= 0.6f;
                if (ReferenceEquals(target, bot.HurtBy) && now - bot.HurtAt < RevengeMs) score *= 0.5f;
            }
            candidates.Add((target, eye, score));
        }

        foreach (IPlayer player in bot.World.Players)
            if (player.Position is Vector3 position && Enemy(bot, player))
                Consider(player, position + Vector3.UnitY * EyeHeight);
        foreach (Bot other in _bots)
            if (Enemy(bot, other))
                Consider(other, other.Npc.EyePosition);

        var ordered = candidates.OrderBy(c => c.Score).ToList();
        object? seen = ordered.FirstOrDefault(c => bot.World.LineOfSight(from, c.Eye)).Target;
        return seen ?? (anywhere ? ordered.FirstOrDefault().Target : null);
    }

    void OnNpcDamaged(INpc npc, IPlayer? by, float amount)
    {
        lock (_sync)
        {
            if (by is not null && _bots.FirstOrDefault(bot => bot.Npc.Id == npc.Id) is { } bot)
            {
                long now = Environment.TickCount64;
                Hurt(bot, by, by.Position, now);
                if (bot.Chat is not null && !bot.Preview && npc.Alive)
                {
                    Queue(bot, bot.Chat.Hurt(by.Name, now), now, quick: true);
                    bot.Partner = by.Actor;
                    bot.PartnerAt = now;
                }
            }
        }
    }

    // По боту попали. Кошмар уклоняется прыжком (Jumps), а сложный и кошмар
    // разворачиваются на стрелявшего, если сейчас не заняты видимой целью, -
    // даже если его не видно: идут туда, откуда стреляли.
    void Hurt(Bot bot, object by, Vector3? from, long now)
    {
        bot.HurtBy = by;
        bot.HurtAt = now;
        bot.Dodged = false;
        if (bot.Preview || !bot.Attacks || !bot.Level.Retaliates || !Enemy(bot, by)) return;
        if (bot.Target is not null && bot.TargetVisible) return;
        if (!ReferenceEquals(bot.Target, by))
        {
            bot.Target = by;
            bot.TargetSince = now;
        }
        if (from is Vector3 at)
        {
            bot.LastSeen = at + Vector3.UnitY * EyeHeight;
            bot.LastSeenAt = now;
        }
        bot.NextRetarget = now + RetargetEveryMs;
    }

    static Vector3? TargetEye(object? target) => target switch
    {
        IPlayer player when player.Alive && !player.Building && player.Position is Vector3 position => position + Vector3.UnitY * EyeHeight,
        Bot other when other.Npc.Exists && other.Npc.Alive => other.Npc.EyePosition,
        _ => null,
    };

    void Fight(Bot bot, Vector3 eye, long now)
    {
        INpc npc = bot.Npc;
        Difficulty level = bot.Level;
        bool wasVisible = bot.TargetVisible;
        bool visible = bot.World.LineOfSight(npc.EyePosition, eye);
        bot.TargetVisible = visible;
        if (visible)
        {
            bot.LastSeen = eye;
            bot.LastSeenAt = now;
            bot.HuntGoal = null;
            bot.AmbushUntil = 0;
        }
        Track(bot, eye, now);
        AvatarItem? threat = WeaponOf(bot.Target);

        // «Безумный» пережидает перезарядку за укрытием и к выстрелу
        // выглядывает обратно, часто прыжком.
        if (level.Cunning && bot.CoverGoal is Vector3 cover)
        {
            if (now < bot.CoverUntil && !(visible && now >= bot.NextShot))
            {
                npc.WalkTo(cover);
                Look(npc, (bot.LastSeen ?? eye) - npc.EyePosition);
                Unstick(bot, now);
                return;
            }
            bot.CoverGoal = null;
            npc.WalkTo(bot.PeekFrom);
            if (npc.Grounded && Random.Shared.NextSingle() < 0.5f) npc.Jump(0.2f + Random.Shared.NextSingle() * 0.3f);
            bot.NextManeuver = now + ApproachReplanMs;
        }

        // Цель скрылась - идёт туда, где видел её последний раз, и смотрит
        // по ходу. Сквозь стены бот цель не видит.
        if (!visible)
        {
            npc.LookYaw = null;
            Vector3 seen = bot.LastSeen ?? eye;
            if (level.Omniscient)
            {
                Stalk(bot, eye, now);
                return;
            }
            if (level.Cunning)
            {
                Hunt(bot, seen, lost: wasVisible, now);
                return;
            }
            if (new Vector2(seen.X - npc.Position.X, seen.Z - npc.Position.Z).Length() > ArriveDistance)
                npc.WalkTo(new Vector3(seen.X, npc.Position.Y, seen.Z));
            else if (npc.Walking) npc.Stop();
            Unstick(bot, now);
            return;
        }

        Vector3 offset = eye - npc.EyePosition;
        float flat = new Vector2(offset.X, offset.Z).Length();
        float distance = offset.Length();

        // кошмар с «все оружия» берёт оружие под дистанцию, «безумный» -
        // ещё и против оружия врага
        if (level.Tactics && Weapons.IsAll(bot.Settings.Weapon) && now >= bot.NextWeaponSwap)
        {
            Weapon best = level.Cunning && threat is AvatarItem enemy ? Weapons.Counter(enemy, distance) : Weapons.ForDistance(distance);
            if (best != bot.Weapon)
            {
                bot.Weapon = best;
                npc.Holding = best.Item;
            }
            bot.NextWeaponSwap = now + WeaponSwapMs;
        }
        Weapon weapon = bot.Weapon!;

        // смотрит на цель, даже когда бежит вбок, - как игрок
        Look(npc, offset);

        bool finishing = level.Cunning && bot.DealtToTarget >= FinishDamage;
        if (level.Cunning) Spread(bot);
        // за стенкой «ультра» стоит: манёвр или прыжок открыли бы тело
        if (!(level.Exploits && HoldGlitch(bot, eye, weapon, finishing, now)))
        {
            Maneuver(bot, offset, flat, weapon, threat, finishing, now);
            Jumps(bot, threat, now);
        }
        Unstick(bot, now);

        float range = Range(bot, weapon);
        if (distance > range || now < bot.NextShot || now - bot.TargetSince < level.ReactionMs) return;

        bot.NextShot = now + (long)(weapon.IntervalMs * level.FireScale);
        // бегущая цель сбивает прицел - если нет упреждения
        float speed = new Vector2(bot.TargetVelocity.X, bot.TargetVelocity.Z).Length();
        float moving = 1f - (1f - level.Lead) * MathF.Min(1f, speed / RunSpeed) * 0.5f;
        float chance = weapon.Melee
            ? Math.Min(1f, level.Accuracy + 0.3f)
            : level.Accuracy * (1f - level.Falloff * distance / range) * moving;
        bool hit = Random.Shared.NextSingle() < chance;

        Vector3 aim = Vector3.Normalize(eye + bot.TargetVelocity * (level.Lead * 0.1f) - npc.EyePosition);
        if (!hit)
        {
            var miss = new Vector3(Random.Shared.NextSingle() - 0.5f, Random.Shared.NextSingle() - 0.5f, Random.Shared.NextSingle() - 0.5f);
            aim = Vector3.Normalize(aim + miss * 0.35f);
        }
        npc.Aim(aim);
        npc.Shoot(aim);
        if (level.Cunning && bot.Glitch is null) TakeCover(bot, eye, weapon, finishing, now);
        if (!hit) return;

        // попадание - как от живого игрока: урон оружия и отбрасывание
        int pellets = weapon.Pellets > 1
            ? Math.Clamp((int)MathF.Round(weapon.Pellets * (1f - distance / weapon.Range)), 1, weapon.Pellets)
            : 1;
        Vector3 away = flat > 0.001f ? new Vector3(offset.X, 0f, offset.Z) / flat : Vector3.Zero;
        Vector3 impulse = (away + Vector3.UnitY * weapon.Lift) * weapon.Impulse * pellets;
        float damage = weapon.Damage * pellets;
        bot.DealtToTarget += damage;
        switch (bot.Target)
        {
            case IPlayer player:
                player.Hit(weapon.Kind, npc, impulse, damage);
                break;
            case Bot other:
                other.Npc.Hit(weapon.Kind, impulse, npc, damage);
                Hurt(other, bot, npc.Position, now);
                break;
        }
    }

    // Скорость цели по её прошлым позициям - для упреждения.
    static void Track(Bot bot, Vector3 eye, long now)
    {
        if (!ReferenceEquals(bot.Tracked, bot.Target))
        {
            bot.Tracked = bot.Target;
            bot.TrackPos = eye;
            bot.TrackAt = now;
            bot.TargetVelocity = Vector3.Zero;
            bot.DealtToTarget = 0f;
            return;
        }
        float seconds = (now - bot.TrackAt) / 1000f;
        if (seconds < 0.05f) return;
        bot.TargetVelocity = Vector3.Lerp(bot.TargetVelocity, (eye - bot.TrackPos) / seconds, 0.5f);
        bot.TrackPos = eye;
        bot.TrackAt = now;
    }

    // Движение в перестрелке, а не бег по прямой: подходит до своей
    // дистанции (кошмар - дугой с фланга, сложный - зигзагом), слишком
    // близко - отходит, на дистанции - бегает вбок, меняя сторону.
    // «Безумный» ещё не входит в дальность оружия врага, если его дальность
    // больше, зигзагом сближается с рейлганом, перерезает цели путь,
    // делает ложные рывки, а добивая - давит вплотную.
    static void Maneuver(Bot bot, Vector3 offset, float flat, Weapon weapon, AvatarItem? threat, bool finishing, long now)
    {
        if (now < bot.NextManeuver) return;
        INpc npc = bot.Npc;
        Difficulty level = bot.Level;
        // раненый кошмар держится на краю дальности
        bool low = level.Tactics && npc.Health < npc.MaxHealth * LowHealth;
        float range = Range(bot, weapon);
        float keep = low && !weapon.Melee ? range * 0.85f : weapon.Keep;
        if (level.Cunning)
        {
            float danger = threat is AvatarItem enemy ? Weapons.ThreatRange(enemy) : 0f;
            if (danger > 0f && range > danger + 3f) keep = MathF.Max(keep, MathF.Min(danger + 2f, range * 0.9f));
            if (finishing && !weapon.Melee) keep = MathF.Min(keep, MathF.Max(3f, weapon.Keep * 0.5f));
        }
        Vector3 toward = flat > 0.001f ? new Vector3(offset.X, 0f, offset.Z) / flat : Vector3.UnitZ;
        var side = new Vector3(-toward.Z, 0f, toward.X);
        Vector3 here = npc.Position;

        if (level.Cunning)
        {
            // один - заходит туда, куда бежит цель; стая - сторону дал Spread
            float drift = Vector3.Dot(bot.TargetVelocity, side);
            if (!bot.Packed && MathF.Abs(drift) > 1f) bot.FlankSide = drift > 0f ? 1 : -1;
        }
        else if (level.Tactics && Random.Shared.NextSingle() < 0.3f) bot.FlankSide = -bot.FlankSide;
        if (Random.Shared.NextSingle() < (level.Cunning ? 0.5f : 0.35f)) bot.StrafeSide = -bot.StrafeSide;

        if (flat > keep)
        {
            float swerve = level.Cunning && threat == AvatarItem.RailGun ? 3f * bot.StrafeSide
                : level.Tactics ? MathF.Min(flat * 0.5f, 6f) * bot.FlankSide
                : Random.Shared.NextSingle() < level.StrafeChance * 0.5f ? 2f * bot.StrafeSide
                : 0f;
            npc.WalkTo(here + toward * MathF.Min(flat - keep * 0.8f + 1f, 10f) + side * swerve);
            bot.NextManeuver = now + ApproachReplanMs;   // цель бежит - поправляем курс чаще
            return;
        }

        if (level.Cunning && !finishing && !weapon.Melee && Random.Shared.NextSingle() < 0.12f)
        {
            // ложный рывок к цели - и сразу в сторону
            npc.WalkTo(here + toward * 3f + side * bot.StrafeSide);
            bot.StrafeSide = -bot.StrafeSide;
            bot.NextManeuver = now + 200;
            return;
        }

        if (!weapon.Melee && flat < keep * 0.45f && level.StrafeChance >= 0.5f)
            npc.WalkTo(here - toward * 4f + side * bot.StrafeSide * 2f);
        else if (low || Random.Shared.NextSingle() < level.StrafeChance)
            npc.WalkTo(here + side * bot.StrafeSide * (2f + Random.Shared.NextSingle() * 2.5f));
        else if (npc.Walking) npc.Stop();

        bot.NextManeuver = now + (level.Cunning ? Random.Shared.Next(150, 500)
            : level.Tactics ? Random.Shared.Next(250, 650)
            : Random.Shared.Next(500, 1200));
    }

    // Прыжки в бою. Кошмар ещё и уклоняется прыжком, когда в него попали.
    // «Безумный» распрыгивает на бегу и сбивает ритм под рейлганом и базукой.
    static void Jumps(Bot bot, AvatarItem? threat, long now)
    {
        INpc npc = bot.Npc;
        Difficulty level = bot.Level;
        if (!npc.Grounded) return;
        if (level.Tactics && !bot.Dodged && now - bot.HurtAt < DodgeWindowMs)
        {
            bot.Dodged = true;
            npc.Jump(0.2f + Random.Shared.NextSingle() * 0.3f);
            bot.StrafeSide = -bot.StrafeSide;
            bot.NextManeuver = now;
            return;
        }
        if (level.Cunning)
        {
            if (threat is AvatarItem.RailGun or AvatarItem.Bazooka && now >= bot.NextRailDodge)
            {
                bot.NextRailDodge = now + Random.Shared.Next(600, 1200);
                if (Random.Shared.NextSingle() < 0.6f) npc.Jump(0.15f + Random.Shared.NextSingle() * 0.35f);
                else
                {
                    bot.StrafeSide = -bot.StrafeSide;
                    bot.NextManeuver = now;
                }
                return;
            }
            if (Bhop(bot, now)) return;
        }
        float rate = level.JumpPerSecond * (level.Tactics && npc.Health < npc.MaxHealth * LowHealth ? 1.5f : 1f);
        if (Random.Shared.NextSingle() < rate * ThinkEveryMs / 1000f)
            npc.Jump(0.1f + Random.Shared.NextSingle() * 0.4f);
    }

    static void Look(INpc npc, Vector3 offset)
    {
        if (new Vector2(offset.X, offset.Z).Length() <= 0.001f) return;
        float yaw = MathF.Atan2(offset.X, offset.Z) * 180f / MathF.PI;
        npc.Face(yaw);
        npc.LookYaw = yaw;
    }

    static float Flat(Vector3 offset) => new Vector2(offset.X, offset.Z).Length();

    // Дальность стрельбы бота: «ультра» бьёт пулей дальше игрока.
    static float Range(Bot bot, Weapon weapon) =>
        Weapons.Ranged(weapon) ? weapon.Range * bot.Level.RangeScale : weapon.Range;

    static AvatarItem? WeaponOf(object? target) => target switch
    {
        IPlayer player => player.Holding,
        Bot other => other.Weapon?.Kind,
        _ => null,
    };

    // Банни-хоп: на бегу прыгает почти сразу после приземления.
    static bool Bhop(Bot bot, long now)
    {
        INpc npc = bot.Npc;
        if (!npc.Walking || !npc.Grounded || now < bot.NextBhop || Random.Shared.NextSingle() > 0.75f) return false;
        bot.NextBhop = now + BhopEveryMs;
        npc.Jump(0.15f + Random.Shared.NextSingle() * 0.35f);
        return true;
    }

    // «Безумный» потерял цель из виду: через раз ждёт в засаде, отступив от
    // угла и глядя туда, где видел цель, или бежит на упреждение - куда она
    // бежала. После засады идёт искать.
    void Hunt(Bot bot, Vector3 seen, bool lost, long now)
    {
        INpc npc = bot.Npc;
        if (lost)
        {
            bot.Ambush = !bot.Ambush;
            if (bot.Ambush)
            {
                Vector3 back = npc.Position - seen;
                back.Y = 0f;
                Vector3 spot = back.LengthSquared() > 0.001f ? npc.Position + Vector3.Normalize(back) * 2f : npc.Position;
                bot.HuntGoal = bot.World.LineOfSight(npc.Position + Waist, spot + Waist) ? spot : npc.Position;
                bot.AmbushUntil = now + AmbushMs;
            }
            else
            {
                var run = new Vector3(bot.TargetVelocity.X, 0f, bot.TargetVelocity.Z);
                if (run.Length() > RunSpeed) run = Vector3.Normalize(run) * RunSpeed;
                bot.HuntGoal = seen + run;   // где цель будет через секунду
            }
        }

        if (now < bot.AmbushUntil && bot.HuntGoal is Vector3 hide)
        {
            if (Flat(hide - npc.Position) > ArriveDistance) npc.WalkTo(hide);
            else if (npc.Walking) npc.Stop();
            Look(npc, seen - npc.EyePosition);
            Unstick(bot, now);
            return;
        }

        Vector3 goal = bot.Ambush ? seen : bot.HuntGoal ?? seen;
        if (Flat(goal - npc.Position) > ArriveDistance) npc.WalkTo(new Vector3(goal.X, npc.Position.Y, goal.Z));
        else if (npc.Walking) npc.Stop();
        Bhop(bot, now);
        Unstick(bot, now);
    }

    // После выстрела из медленного оружия (а раненый или под огнём двоих -
    // и из быстрого, урывками) «безумный» уходит за укрытие до следующего
    // выстрела. Добивая цель - не прячется.
    void TakeCover(Bot bot, Vector3 eye, Weapon weapon, bool finishing, long now)
    {
        INpc npc = bot.Npc;
        if (finishing || weapon.Melee) return;
        bool low = npc.Health < npc.MaxHealth * CoverHealth;
        bool slow = Weapons.Slow(weapon);
        if (slow && !low && Vector3.Distance(eye, npc.EyePosition) < CoverCloseDistance) return;
        if (!slow && !(low && Random.Shared.NextSingle() < 0.15f) && !(bot.Threats >= 2 && Random.Shared.NextSingle() < 0.1f)) return;
        if (FindCover(bot, eye) is not Vector3 cover) return;
        bot.CoverGoal = cover;
        bot.PeekFrom = npc.Position;
        bot.CoverUntil = slow ? bot.NextShot - PeekLeadMs : now + Random.Shared.Next(700, 1200);
    }

    // Укрытие рядом: точка в 3-6 м, откуда врага не видно, путь до неё
    // открыт и под ней есть пол (чтобы не спрыгнуть с обрыва). Лучше
    // ближние и те, что дальше от врага.
    Vector3? FindCover(Bot bot, Vector3 threatEye)
    {
        INpc npc = bot.Npc;
        Vector3 here = npc.Position;
        Vector3 eyeLift = npc.EyePosition - here;
        Vector3 away = here - threatEye;
        away.Y = 0f;
        away = away.LengthSquared() > 0.001f ? Vector3.Normalize(away) : Vector3.Zero;

        Vector3? best = null;
        float bestScore = float.MaxValue;
        foreach (float radius in CoverRadii)
            for (int i = 0; i < CoverDirections; i++)
            {
                float angle = i * MathF.Tau / CoverDirections;
                var direction = new Vector3(MathF.Cos(angle), 0f, MathF.Sin(angle));
                Vector3 spot = here + direction * radius;
                float score = radius - Vector3.Dot(direction, away) * 2f;
                if (score >= bestScore) continue;
                if (!bot.World.LineOfSight(here + Waist, spot + Waist)) continue;              // путь загорожен
                if (bot.World.LineOfSight(spot + Waist, spot - Vector3.UnitY * 1.5f)) continue; // пола нет
                if (bot.World.LineOfSight(spot + eyeLift, threatEye)) continue;               // оттуда видно врага
                best = spot;
                bestScore = score;
            }
        return best;
    }

    // «Ультра безумие» знает, где цель, даже когда её не видно, но не бежит
    // на неё по прямой, как зомби: заходит на точку, откуда её видно, сбоку
    // или со спины, короткими отрезками и прыжками. Цель сама бежит к нему -
    // ждёт за углом, глядя туда, откуда она выйдет.
    void Stalk(Bot bot, Vector3 eye, long now)
    {
        INpc npc = bot.Npc;
        Weapon weapon = bot.Weapon!;
        Vector3 here = npc.Position;
        Spread(bot);

        if (bot.Level.Exploits && HoldGlitch(bot, eye, weapon, finishing: false, now))
        {
            Look(npc, eye - npc.EyePosition);
            Unstick(bot, now);
            return;
        }

        Vector3 toBot = here - eye;
        toBot.Y = 0f;
        float apart = toBot.Length();
        float closing = apart > 0.01f ? Vector3.Dot(bot.TargetVelocity, toBot / apart) : 0f;
        if (now >= bot.WaitUntil && now >= bot.NextWait && closing > ClosingSpeed && apart < 30f)
        {
            bot.WaitUntil = now + StalkWaitMs;
            bot.NextWait = bot.WaitUntil + StalkWaitPauseMs;
            bot.HuntGoal = FindCover(bot, eye) ?? here;
        }
        if (now < bot.WaitUntil && bot.HuntGoal is Vector3 hide)
        {
            if (Flat(hide - here) > ArriveDistance) npc.WalkTo(hide);
            else if (npc.Walking) npc.Stop();
            Look(npc, eye - npc.EyePosition);
            Unstick(bot, now);
            return;
        }

        bool arrived = bot.Approach is Vector3 goal && Flat(goal - here) <= ArriveDistance;
        if (bot.Approach is null || arrived || now >= bot.NextApproach)
        {
            float keep = MathF.Max(weapon.Keep, GlitchMinDistance);
            // бьёт дальше оружия игрока - выглядывает оттуда, куда ответ не долетит
            if (Weapons.Ranged(weapon) && bot.Level.RangeScale > 1f)
            {
                float danger = WeaponOf(bot.Target) is AvatarItem enemy ? Weapons.ThreatRange(enemy) : 0f;
                keep = Math.Clamp(danger + 3f, keep, MathF.Max(keep, MathF.Min(Range(bot, weapon) * 0.8f, StalkFar)));
            }
            bot.Approach = PeekSpot(bot, eye, keep) ?? FlankPoint(bot, eye, keep);
            bot.NextApproach = now + StalkReplanMs;
        }
        Vector3 way = bot.Approach.Value - here;
        way.Y = 0f;
        float length = way.Length();
        if (length > ArriveDistance) npc.WalkTo(here + way / length * MathF.Min(length, StalkStep));
        else if (npc.Walking) npc.Stop();
        Bhop(bot, now);
        Unstick(bot, now);
    }

    // Точка подхода: на дистанции keep от цели, откуда её видно и где есть
    // пол. Лучше сбоку или со спины (против того, куда она бежит), со своей
    // стороны фланга, ближе к боту и не в лоб.
    Vector3? PeekSpot(Bot bot, Vector3 eye, float keep)
    {
        INpc npc = bot.Npc;
        Vector3 here = npc.Position;
        Vector3 eyeLift = npc.EyePosition - here;
        Vector3 feet = eye - Vector3.UnitY * EyeHeight;
        var run = new Vector3(bot.TargetVelocity.X, 0f, bot.TargetVelocity.Z);
        Vector3 heading = run.Length() > 1f ? Vector3.Normalize(run) : Vector3.Zero;
        Vector3 toBot = here - feet;
        toBot.Y = 0f;
        toBot = toBot.LengthSquared() > 0.001f ? Vector3.Normalize(toBot) : Vector3.UnitZ;
        Vector3 flank = new Vector3(-toBot.Z, 0f, toBot.X) * bot.FlankSide;

        Vector3? best = null;
        float bestScore = float.MaxValue;
        for (int i = 0; i < PeekDirections; i++)
        {
            float angle = i * MathF.Tau / PeekDirections;
            var direction = new Vector3(MathF.Cos(angle), 0f, MathF.Sin(angle));
            Vector3 spot = feet + direction * keep;
            float score = Vector3.Distance(here, spot) * 0.15f
                + Vector3.Dot(direction, heading) * 2f            // сзади цели - лучше
                - Vector3.Dot(direction, flank) * 1.5f            // своя сторона фланга
                + MathF.Max(0f, Vector3.Dot(direction, toBot) - 0.5f) * 3f;   // в лоб - хуже
            if (score >= bestScore) continue;
            if (!bot.World.LineOfSight(spot + eyeLift, eye)) continue;                     // оттуда цель не видно
            if (bot.World.LineOfSight(spot + Waist, spot - Vector3.UnitY * 1.5f)) continue; // пола нет
            best = spot;
            bestScore = score;
        }
        return best;
    }

    // Точки подхода нет - дуга с фланга: под 60° от линии бот-цель.
    static Vector3 FlankPoint(Bot bot, Vector3 eye, float keep)
    {
        Vector3 feet = eye - Vector3.UnitY * EyeHeight;
        Vector3 toBot = bot.Npc.Position - feet;
        toBot.Y = 0f;
        toBot = toBot.LengthSquared() > 0.001f ? Vector3.Normalize(toBot) : Vector3.UnitZ;
        float angle = MathF.PI / 3f * bot.FlankSide;
        var direction = new Vector3(toBot.X * MathF.Cos(angle) - toBot.Z * MathF.Sin(angle), 0f,
            toBot.X * MathF.Sin(angle) + toBot.Z * MathF.Cos(angle));
        return feet + direction * keep;
    }

    // Баг игры (видео пользователя): вплотную за стенкой ростом примерно по
    // голову тело закрыто, а глаза - и выстрел, он идёт от глаз, - уже над
    // ней. В ответ игрок попадает разве что в макушку. «Ультра» встаёт за
    // такую стенку, как только видит цель, и бьёт оттуда, не бегая и не прыгая.
    bool HoldGlitch(Bot bot, Vector3 eye, Weapon weapon, bool finishing, long now)
    {
        INpc npc = bot.Npc;
        if (weapon.Melee || finishing)
        {
            bot.Glitch = null;
            return false;
        }
        if (now >= bot.NextGlitchSearch)
        {
            bot.Glitch = GlitchSpot(bot, eye, weapon);
            bot.NextGlitchSearch = now + GlitchSearchMs;
        }
        if (bot.Glitch is not Vector3 spot) return false;
        if (Flat(spot - npc.Position) > ArriveDistance * 0.5f) npc.WalkTo(spot);
        else if (npc.Walking) npc.Stop();
        return true;
    }

    // Место для бага рядом с ботом (сначала - где стоит): глаза видят цель,
    // грудь закрыта стенкой, и стенка в паре метров впереди, а не у цели.
    Vector3? GlitchSpot(Bot bot, Vector3 eye, Weapon weapon)
    {
        INpc npc = bot.Npc;
        Vector3 here = npc.Position;
        Vector3 eyeLift = npc.EyePosition - here;
        Vector3 chest = eyeLift * ChestShare;
        foreach (float radius in GlitchRadii)
            for (int i = 0; i < (radius == 0f ? 1 : CoverDirections); i++)
            {
                float angle = i * MathF.Tau / CoverDirections;
                Vector3 spot = here + new Vector3(MathF.Cos(angle), 0f, MathF.Sin(angle)) * radius;
                float distance = Vector3.Distance(spot + eyeLift, eye);
                if (distance < GlitchMinDistance || distance > Range(bot, weapon) * 0.95f) continue;
                if (radius > 0f && (!bot.World.LineOfSight(here + Waist, spot + Waist)
                    || bot.World.LineOfSight(spot + Waist, spot - Vector3.UnitY * 1.5f))) continue;
                if (!bot.World.LineOfSight(spot + eyeLift, eye)) continue;   // поверх стенки цель не видно
                if (bot.World.LineOfSight(spot + chest, eye)) continue;      // грудь открыта
                Vector3 ahead = eye - (spot + chest);
                ahead.Y = 0f;
                if (ahead.LengthSquared() < 0.001f) continue;
                ahead = Vector3.Normalize(ahead);
                if (bot.World.LineOfSight(spot + chest, spot + chest + ahead * GlitchWallReach)) continue;   // стенка далеко
                return spot;
            }
        return null;
    }

    // «Безумные» одной команды на одной цели заходят с разных сторон.
    void Spread(Bot bot)
    {
        List<Bot> pack = bot.Team is null ? [bot]
            : [.. _bots.Where(mate => mate.Team == bot.Team && mate.Level.Cunning && ReferenceEquals(mate.Target, bot.Target))];
        bot.Packed = pack.Count > 1;
        if (bot.Packed) bot.FlankSide = pack.IndexOf(bot) % 2 == 0 ? 1 : -1;
    }

    // Сколько противников сейчас видят бота и достают до него.
    int CountThreats(Bot bot)
    {
        Vector3 eye = bot.Npc.EyePosition;
        int count = 0;
        foreach (IPlayer player in bot.World.Players)
            if (player.Position is Vector3 position && Enemy(bot, player)
                && Vector3.Distance(position, eye) <= Weapons.ThreatRange(player.Holding)
                && bot.World.LineOfSight(position + Vector3.UnitY * EyeHeight, eye))
                count++;
        foreach (Bot other in _bots)
            if (Enemy(bot, other) && other.Attacks && other.Weapon is Weapon weapon
                && Vector3.Distance(other.Npc.EyePosition, eye) <= weapon.Range
                && bot.World.LineOfSight(other.Npc.EyePosition, eye))
                count++;
        return count;
    }

    // --- «общается» ----------------------------------------------------------

    // Игрок написал в чат. Отвечает один бот: тот, кого назвали по нику,
    // иначе тот, с кем игрок уже разговаривает, иначе ближайший, кто слышит.
    void OnChat(ChatMessage message)
    {
        try
        {
            lock (_sync)
            {
                IPlayer player = message.Player;
                string text = message.Text?.Trim() ?? "";
                if (text.Length == 0 || player.Position is not Vector3 at) return;
                long now = Environment.TickCount64;
                List<Bot> talkers = [.. _bots.Where(bot => bot.Chat is not null && !bot.Preview && bot.Npc.Exists && bot.Npc.Alive
                    && bot.World.Id == player.World.Id)];
                float Apart(Bot bot) => Vector3.Distance(bot.Npc.Position, at);
                Bot? bot = talkers.FirstOrDefault(bot => bot.Settings.Name is { Length: >= 2 } name && text.Contains(name, StringComparison.OrdinalIgnoreCase))
                    ?? talkers.Where(bot => bot.Partner == player.Actor && now - bot.PartnerAt < ChatHoldMs && Apart(bot) <= ChatKeepDistance).MinBy(Apart)
                    ?? talkers.Where(bot => Apart(bot) <= HearDistance).MinBy(Apart);
                if (bot is null) return;

                Queue(bot, bot.Chat!.Reply(player.Name, text, now), now, quick: false);
                bot.Partner = player.Actor;
                // попрощались - постоит, договорит и пойдёт гулять
                bot.PartnerAt = bot.Chat.SaidBye(player.Name) ? now - ChatHoldMs + 4000 : now;
            }
        }
        catch (Exception error)
        {
            Console.WriteLine($"kogamaz-npc: chat failed: {error}");
        }
    }

    // Реплика уходит не сразу: бот будто печатает - чем длиннее, тем дольше.
    // Новая реплика заменяет недосказанную.
    static void Queue(Bot bot, string? line, long now, bool quick)
    {
        if (line is null) return;
        bot.Pending = line;
        bot.PendingAt = now + (quick ? Random.Shared.Next(300, 700) : Math.Min(2500, 800 + line.Length * 25));
    }

    // В разговоре стоит лицом к собеседнику, отстал - подходит. Подошёл
    // игрок - здоровается первым. Не с кем говорить - гуляет.
    void Converse(Bot bot, long now)
    {
        INpc npc = bot.Npc;
        if (bot.Pending is string line && now >= bot.PendingAt)
        {
            npc.Say(line);
            bot.Pending = null;
        }

        IPlayer? partner = now - bot.PartnerAt < ChatHoldMs
            ? bot.World.Players.FirstOrDefault(player => player.Actor == bot.Partner && player.Alive && player.Position is not null)
            : null;
        if (partner?.Position is Vector3 at && Flat(at - npc.Position) > ChatKeepDistance) partner = null;

        if (partner is null && bot.Pending is null && now >= bot.NextGreetCheck)
        {
            bot.NextGreetCheck = now + GreetCheckMs;
            IPlayer? near = bot.World.Players
                .Where(player => player.Alive && !player.Building && player.Position is Vector3 position
                    && Vector3.Distance(position, npc.Position) <= GreetDistance)
                .MinBy(player => Vector3.Distance(player.Position!.Value, npc.Position));
            if (near is not null && bot.Chat!.Greet(near.Name, now) is string hello)
            {
                Queue(bot, hello, now, quick: false);
                bot.Partner = near.Actor;
                bot.PartnerAt = now;
                partner = near;
            }
        }

        if (partner?.Position is Vector3 there)
        {
            Vector3 offset = there + Vector3.UnitY * EyeHeight - npc.EyePosition;
            float flat = Flat(offset);
            if (flat > TalkFar)
            {
                Vector3 stop = there - new Vector3(offset.X, 0f, offset.Z) / flat * TalkNear;
                npc.WalkTo(new Vector3(stop.X, npc.Position.Y, stop.Z));
            }
            else if (npc.Walking) npc.Stop();
            Look(npc, offset);
            Unstick(bot, now);
            return;
        }
        Wander(bot, now);
    }

    // «Просто ходит»: гуляет вокруг своей точки с паузами, иногда подпрыгивает.
    void Wander(Bot bot, long now)
    {
        INpc npc = bot.Npc;
        npc.LookYaw = null;
        if (npc.Walking && npc.Grounded && Random.Shared.NextSingle() < WanderJumpChance) npc.Jump(0.2f);
        if (now < bot.PauseUntil) return;

        bool arrived = bot.WanderGoal is Vector3 goal
            && new Vector2(goal.X - npc.Position.X, goal.Z - npc.Position.Z).Length() <= ArriveDistance;
        if (bot.WanderGoal is null || arrived || now > bot.WanderUntil)
        {
            if (npc.Walking) npc.Stop();
            if (arrived)
            {
                bot.WanderGoal = null;
                bot.PauseUntil = now + Random.Shared.Next(1000, 3500);
                return;
            }
            float angle = Random.Shared.NextSingle() * MathF.Tau;
            float radius = Random.Shared.NextSingle() * WanderRadius;
            bot.WanderGoal = bot.Home + new Vector3(MathF.Cos(angle) * radius, 0f, MathF.Sin(angle) * radius);
            bot.WanderUntil = now + WanderGiveUpMs;
            npc.WalkTo(bot.WanderGoal.Value);
        }
        Unstick(bot, now);
    }

    static void Unstick(Bot bot, long now)
    {
        if (now - bot.LastCheckAt < StuckCheckMs) return;
        INpc npc = bot.Npc;
        Vector3 moved = npc.Position - bot.LastCheck;
        moved.Y = 0f;
        if (npc.Walking && moved.Length() < StuckDistance && npc.Grounded) npc.Jump(StuckJump);
        bot.LastCheck = npc.Position;
        bot.LastCheckAt = now;
    }
}
