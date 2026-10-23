using System.Numerics;

namespace OpenKogama.Api;

public interface INpc
{
    int Id { get; }
    int ObjectId { get; }
    string Name { get; }
    Vector3 Position { get; }
    Vector3 EyePosition { get; }
    float Yaw { get; }
    string Animation { get; }
    AvatarAnimation AnimationState => Enum.TryParse(Animation, ignoreCase: true, out AvatarAnimation state) ? state : AvatarAnimation.Unknown;
    float Speed { get; set; }
    float Size { get; }
    int Level { get; }
    // KoGaMaZ: команда бота, null - сам за себя. Игроки этой команды его не ранят.
    Team? Team { get; set; }
    // KoGaMaZ: куда смотреть, даже на бегу (стрейф как у игрока); null - по шагу.
    float? LookYaw { get; set; }
    bool OnFire { get; set; }
    string? Holding { get; set; }
    float MaxHealth { get; set; }
    bool Walking { get; }
    bool Grounded { get; }
    float Health { get; }
    bool Alive { get; }
    bool Exists { get; }

    void WalkTo(Vector3 target);
    void Stop();
    void Jump(float holdSeconds = 0.5f);
    void Teleport(Vector3 position, float? yaw = null);
    void Face(float yaw);
    void Shoot(Vector3 direction);
    void Shoot(Vector3 direction, float holdSeconds);
    void Aim(Vector3 direction);
    bool Hold(IWorldObject weapon);
    bool BecomeColossus(IWorldObject trigger);
    bool BecomeColossus(ColossusSize size);
    void PlayAnimation(string? state);
    void PlayAnimation(AvatarAnimation state) => PlayAnimation(state == AvatarAnimation.Unknown ? null : state.ToString());
    void Damage(float amount, IPlayer? by = null);
    // KoGaMaZ: попадание из оружия, как от игрока: урон, отбрасывание и
    // причина смерти этого оружия.
    void Hit(AvatarItem weapon, Vector3 impulse, INpc? by = null, float? damage = null);
    void Respawn(Vector3 position);
    // KoGaMaZ: сказать в общий чат - строка подписана ником бота.
    void Say(string text);
    void Remove();
}
