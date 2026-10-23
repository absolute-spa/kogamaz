using System.Text.Json;
using System.Text.Json.Serialization;

namespace KogamazNpc;

// Настройки одного NPC. Пишет их лаунчер (окно «NPC» у карты), плагин только
// читает. Значения - строки, а не enum: файл правят руками, и опечатка в
// одном поле не должна ронять разбор всего файла - неизвестное станет
// значением по умолчанию.
public sealed class NpcSettings
{
    public string? Name { get; set; }
    public string Mode { get; set; } = "attack";        // attack | walk | talk | idle
    public string Weapon { get; set; } = "all";         // all | none | CenterGun | Shotgun | ...
    public string Team { get; set; } = "none";          // none | blue | red | green | yellow (class - команда класса ранней версии)
    public string Difficulty { get; set; } = "normal";  // easy | normal | hard | nightmare | insane | ultra
    public float Respawn { get; set; } = 5f;            // секунд до возрождения
    public string? Avatar { get; set; }                 // облик: файл .kgavatar в data/kogamaz-npc-avatars, null - обычный
    public string? AvatarName { get; set; }             // имя исходного файла - только для окна лаунчера

    // Отпечаток для сравнения: поменялась настройка - бота пересоздаём.
    public string Key() => $"{Name}|{Mode}|{Weapon}|{Team}|{Difficulty}|{Respawn}|{Avatar}";
}

public sealed class WorldSettings
{
    public Dictionary<string, NpcSettings> Npcs { get; set; } = [];
}

public sealed class ConfigFile
{
    public Dictionary<string, WorldSettings> Worlds { get; set; } = [];
}

// Маркеры, которые плагин нашёл в режиме строительства. Лаунчер по ним
// показывает список NPC карты: какие стоят и где.
public sealed class MarkerInfo
{
    public int Id { get; set; }
    public float X { get; set; }
    public float Y { get; set; }
    public float Z { get; set; }
    public string? Name { get; set; }   // из подписи $$npc_Вася, если есть
    public bool Figure { get; set; }    // NPC-класс ранней версии, а не маркер $$npc
    public string? Team { get; set; }   // команда из окна такого класса
}

// Только что поставленный NPC: лаунчер открывает на нём окно настроек.
public sealed class OpenRequest
{
    public int World { get; set; }
    public string? Title { get; set; }
    public int Marker { get; set; }
    public DateTime At { get; set; }
}

public sealed class WorldMarkers
{
    public string? Title { get; set; }
    public List<MarkerInfo> Markers { get; set; } = [];
}

public sealed class MarkersFile
{
    public Dictionary<string, WorldMarkers> Worlds { get; set; } = [];
}

static class Json
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static T Read<T>(string path) where T : new()
    {
        try
        {
            return File.Exists(path) ? JsonSerializer.Deserialize<T>(File.ReadAllText(path), Options) ?? new T() : new T();
        }
        catch (Exception error) when (error is JsonException or IOException)
        {
            Console.WriteLine($"kogamaz-npc: cannot read {Path.GetFileName(path)}: {error.Message}");
            return new T();
        }
    }

    public static void Write<T>(string path, T value)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path + ".tmp", JsonSerializer.Serialize(value, Options));
        File.Move(path + ".tmp", path, overwrite: true);
    }
}
