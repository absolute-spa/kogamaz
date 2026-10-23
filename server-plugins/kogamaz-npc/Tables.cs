using OpenKogama.Api;

namespace KogamazNpc;

// Оружие бота. Попадание уходит клиенту тем же сообщением, что и от живого
// игрока (IPlayer.Hit), поэтому урон - как у игрока с этим оружием, а
// отбрасывание - импульс, который считает клиент. Дальность и темп
// стрельбы сервер не знает - они примерно как у игрока.
public sealed record Weapon(
    AvatarItem Kind,      // его видят клиенты в руках бота
    float Range,          // с какой дистанции стреляет
    float Keep,           // на какую дистанцию подходит к цели
    int IntervalMs,       // пауза между выстрелами
    float Damage,         // урон попадания (у дробовика - одной дробинки)
    float Impulse,        // сила отбрасывания цели
    float Lift = 0f,      // доля импульса вверх
    int Pellets = 1,      // дробинок, попадающих в упор
    bool Melee = false)
{
    public string Item => Kind.ToString();
}

public static class Weapons
{
    static Weapon Of(AvatarItem kind, float range, float keep, int interval, float damage, float impulse,
        float lift = 0f, int pellets = 1, bool melee = false) =>
        new(kind, range, keep, interval, damage, impulse, lift, pellets, melee);

    public static readonly IReadOnlyDictionary<string, Weapon> All = new[]
    {
        Of(AvatarItem.CenterGun, 25f, 12f, 150, 11.5f, 60f),
        Of(AvatarItem.Shotgun, 9f, 5f, 900, 13f, 40f, lift: 0.15f, pellets: 6),
        Of(AvatarItem.RailGun, 45f, 20f, 1800, 100f, 300f, lift: 0.2f),
        Of(AvatarItem.Bazooka, 30f, 14f, 1600, 50f, 900f, lift: 0.5f),
        Of(AvatarItem.SixShooter, 25f, 12f, 500, 12.5f, 60f),
        Of(AvatarItem.DoubleSixShooter, 20f, 10f, 300, 12.5f, 50f),
        Of(AvatarItem.Flamethrower, 7f, 4f, 200, 0f, 0f),
        Of(AvatarItem.ImpulseGun, 15f, 8f, 800, 0f, 1200f, lift: 0.3f),
        Of(AvatarItem.Sword, 2.2f, 1.2f, 700, 15f, 150f, lift: 0.2f, melee: true),
    }.ToDictionary(weapon => weapon.Item, StringComparer.OrdinalIgnoreCase);

    // «Все оружия» у тактика: оружие под дистанцию до цели.
    public static Weapon ForDistance(float distance) =>
        distance <= 3f ? All[nameof(AvatarItem.Shotgun)]
        : distance <= 15f ? All[nameof(AvatarItem.CenterGun)]
        : All[nameof(AvatarItem.RailGun)];

    // «Безумный» с «все оружия» выбирает против оружия врага: ближнему бою не
    // даёт подойти, против рейлгана сам лезет вплотную.
    public static Weapon Counter(AvatarItem enemy, float distance) => enemy switch
    {
        AvatarItem.Shotgun or AvatarItem.Sword or AvatarItem.Flamethrower =>
            distance <= 18f ? All[nameof(AvatarItem.CenterGun)] : All[nameof(AvatarItem.RailGun)],
        AvatarItem.RailGun => distance <= 6f ? All[nameof(AvatarItem.Shotgun)] : All[nameof(AvatarItem.CenterGun)],
        _ => ForDistance(distance),
    };

    // Дальность оружия врага; без оружия (или неизвестное) - 0.
    public static float ThreatRange(AvatarItem enemy) =>
        All.TryGetValue(enemy.ToString(), out Weapon? weapon) ? weapon.Range : 0f;

    // Медленное дальнобойное оружие: после выстрела есть время спрятаться
    // (дробовику в упор прятаться некогда).
    public static bool Slow(Weapon weapon) => weapon.IntervalMs >= 800 && weapon.Range >= 15f;

    // Бьёт пулей, а не в упор: дробь, меч и огнемёт дальше не достают, как
    // ни целься.
    public static bool Ranged(Weapon weapon) =>
        !weapon.Melee && weapon.Pellets == 1 && weapon.Kind != AvatarItem.Flamethrower;

    // «Все оружия»: каждое возрождение и раз в SwitchEveryMs - случайное.
    public const int SwitchEveryMs = 20000;

    public static Weapon? Pick(string choice)
    {
        if (string.Equals(choice, "none", StringComparison.OrdinalIgnoreCase)) return null;
        if (All.TryGetValue(choice, out Weapon? weapon)) return weapon;
        return Random();
    }

    public static Weapon Random()
    {
        Weapon[] list = [.. All.Values];
        return list[System.Random.Shared.Next(list.Length)];
    }

    public static bool IsAll(string choice) => !All.ContainsKey(choice)
        && !string.Equals(choice, "none", StringComparison.OrdinalIgnoreCase);
}

// Уровень сложности. Здоровье и урон у бота всегда как у игрока (100 HP,
// урон оружия) - сложность в том, как он играет: реакция, меткость, темп
// стрельбы, движение в бою и тактика.
public sealed record Difficulty(
    int ReactionMs,       // через сколько после появления цели начинает стрелять
    float Accuracy,       // шанс попасть в упор
    float Falloff,        // насколько меткость падает к краю дальности
    float FireScale,      // множитель паузы между выстрелами (больше - реже)
    float SpeedScale,
    float Sight,          // дальность обзора
    float StrafeChance,   // как часто в бою бегает вбок, а не стоит
    float JumpPerSecond,  // прыжков в секунду в бою
    float Lead,           // упреждение: 1 - бегущая цель не мешает попадать
    bool Retaliates,      // разворачивается на того, кто в него попал
    bool Tactics,         // фланг, уклонение, охота, оружие под дистанцию, фокус
    bool Cunning = false,    // «безумный»: укрытия, засады, финты, банни-хоп, контр-оружие
    bool Omniscient = false, // «ультра»: всегда знает, где цель, и не гуляет
    bool Exploits = false,   // «ультра»: стреляет поверх стенки, пряча тело за ней (баг игры)
    float RangeScale = 1f);  // во сколько раз дальше оружия игрока стреляет пулей (Weapons.Ranged)

public static class Difficulties
{
    public static Difficulty Of(string name) => name.ToLowerInvariant() switch
    {
        "easy" => new(900, 0.30f, 0.6f, 1.6f, 0.85f, 25f, 0f, 0f, 0f, false, false),
        "hard" => new(300, 0.70f, 0.4f, 1.05f, 1.0f, 50f, 0.8f, 0.25f, 0.7f, true, false),
        "nightmare" => new(120, 0.95f, 0.15f, 1.0f, 1.1f, 80f, 1f, 0.6f, 1f, true, true),
        "insane" => new(70, 0.98f, 0.05f, 1.0f, 1.15f, 100f, 1f, 1.2f, 1f, true, true, Cunning: true),
        // темп стрельбы - как у игрока; бьёт втрое дальше, 1.5% промахов
        // только у самого края этой дальности
        "ultra" => new(40, 1.0f, 0.015f, 1.0f, 1.2f, 250f, 1f, 1.5f, 1f, true, true, Cunning: true, Omniscient: true, Exploits: true,
            RangeScale: 3f),
        _ => new(550, 0.50f, 0.5f, 1.25f, 1.0f, 35f, 0.3f, 0.05f, 0.3f, false, false),
    };
}

public static class Teams
{
    // null - «сам за себя»
    public static Team? Of(string name) => name.ToLowerInvariant() switch
    {
        "blue" => Team.Blue,
        "red" => Team.Red,
        "green" => Team.Green,
        "yellow" => Team.Yellow,
        _ => null,
    };

    // Команды, которой нет на карте, клиент у бота не покажет - тогда она
    // видна в нике.
    public static string Tag(Team team) => team switch
    {
        Team.Blue => "[BLUE]",
        Team.Red => "[RED]",
        Team.Green => "[GREEN]",
        _ => "[YELLOW]",
    };
}
