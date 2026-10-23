using System.Text.RegularExpressions;

namespace KogamazNpc;

// Собеседник «общительного» бота - без нейросети: узнаёт, о чём ему пишут
// (приветствие, вопрос, комплимент, грубость...), и отвечает из наборов
// фраз. Подстраивается под каждого игрока: помнит его имя и что он любит,
// копит к нему настроение (комплименты - теплее, выстрелы - холоднее),
// отвечает на его языке и в его манере - коротко и строчными или полно.
// Память живёт, пока работает сервер.
sealed partial class Chatter(string name)
{
    const int MoodMin = -3;
    const int MoodMax = 3;
    const long MoodHealMs = 120000;   // настроение раз в столько возвращается на шаг к нулю
    const long GreetEveryMs = 60000;
    const long HurtEveryMs = 5000;
    const int Remember = 10;          // последних своих фраз не повторяет
    const int MaxLength = 140;

    sealed class Person(string nick)
    {
        public string Nick => nick;
        public int Talks;
        public int Mood;
        public long MoodAt;
        public string? Called;          // «меня зовут ...»
        public readonly List<string> Likes = [];
        public readonly List<string> Dislikes = [];
        public string? Topic;
        public bool Russian = true;
        public bool Casual;
        public bool Smiley;
        public long GreetedAt = long.MinValue / 2;
        public long HurtAt = long.MinValue / 2;
        public int Kills;               // сколько раз убил бота
        public bool Bye;
    }

    readonly Dictionary<string, Person> _people = new(StringComparer.OrdinalIgnoreCase);
    readonly Queue<string> _recent = new();

    public string Name { get; set; } = name;

    // --- что пишет игрок --------------------------------------------------

    [GeneratedRegex(@"\p{IsCyrillic}")]
    private static partial Regex Cyrillic();

    [GeneratedRegex(@"(?:как\s+меня\s+зовут|помнишь,?\s+как\s+меня|what'?s\s+my\s+name|remember\s+my\s+name)", RegexOptions.IgnoreCase)]
    private static partial Regex AskMyName();

    [GeneratedRegex(@"(?:что\s+я\s+(?:люблю|обожаю)|что\s+мне\s+нравится|what\s+do\s+i\s+(?:like|love))", RegexOptions.IgnoreCase)]
    private static partial Regex AskMyLikes();

    [GeneratedRegex(@"(?:меня\s+зовут|мо[её]\s+имя|зови\s+меня|my\s+name\s+is|call\s+me)\s+([\p{L}\d_-]{2,20})", RegexOptions.IgnoreCase)]
    private static partial Regex Introduce();

    [GeneratedRegex(@"(?:я\s+не\s+люблю|ненавижу|мне\s+не\s+нрав(?:ится|ятся)|i\s+(?:don'?t|do\s+not)\s+like|i\s+hate)\s+(.{2,40})", RegexOptions.IgnoreCase)]
    private static partial Regex Dislike();

    [GeneratedRegex(@"(?:я\s+(?:очень\s+)?люблю|мне\s+нрав(?:ится|ятся)|обожаю|i\s+(?:really\s+)?(?:love|like)|i'?m\s+into)\s+(.{2,40})", RegexOptions.IgnoreCase)]
    private static partial Regex Like();

    [GeneratedRegex(@"(?:дура|тупой|тупица|идиот|\bлох|\bнуб|отсто|заткнись|бесишь|урод|stupid|idiot|\bnoob|\bdumb|shut\s+up|loser|\bsucks?\b|\btrash\b)", RegexOptions.IgnoreCase)]
    private static partial Regex Insult();

    [GeneratedRegex(@"(?:молодец|красав|\bкрут|классн|лучший|умный|умница|хороший|nice|\bcool\b|awesome|good\s+bot|great|smart|the\s+best|love\s+you)", RegexOptions.IgnoreCase)]
    private static partial Regex Compliment();

    [GeneratedRegex(@"(?:спасибо|\bспс\b|благодар|thanks|thank\s+you|\bthx\b|\bty\b)", RegexOptions.IgnoreCase)]
    private static partial Regex Thanks();

    [GeneratedRegex(@"(?:прости|извини|сорян|сорри|\bsorry\b|my\s+bad)", RegexOptions.IgnoreCase)]
    private static partial Regex Apology();

    [GeneratedRegex(@"(?:привет|здравствуй|здрасьте|здаров|\bхай\b|\bку\b|добр(?:ый|ое)\s+(?:день|вечер|утро)|салам|\bhello\b|\bhi\b|\bhey\b|\byo\b|\bsup\b)", RegexOptions.IgnoreCase)]
    private static partial Regex Greeting();

    [GeneratedRegex(@"(?:\bпока\b|до\s+свидания|бывай|увидимся|споки|\bbye\b|see\s+you|\bcya\b|goodbye|good\s+night)", RegexOptions.IgnoreCase)]
    private static partial Regex Farewell();

    [GeneratedRegex(@"(?:как\s+дела|как\s+ты\b|как\s+жизнь|как\s+сам|ч[её]\s+как|как\s+поживаешь|how\s+are\s+you|how'?s\s+it\s+going|what'?s\s+up|wassup)", RegexOptions.IgnoreCase)]
    private static partial Regex HowAreYou();

    [GeneratedRegex(@"(?:как\s+тебя\s+зовут|тво[её]\s+имя|кто\s+ты|ты\s+кто|what'?s\s+your\s+name|who\s+are\s+you)", RegexOptions.IgnoreCase)]
    private static partial Regex WhoAreYou();

    [GeneratedRegex(@"(?:ты\s+бот|ты\s+робот|ты\s+живой|ты\s+человек|are\s+you\s+(?:a\s+)?(?:bot|human|real))", RegexOptions.IgnoreCase)]
    private static partial Regex AreYouBot();

    [GeneratedRegex(@"(?:что\s+делаешь|ч[её]\s+делаешь|чем\s+занят|what\s+are\s+you\s+doing|\bwyd\b)", RegexOptions.IgnoreCase)]
    private static partial Regex Doing();

    [GeneratedRegex(@"(?:шутк|анекдот|рассмеши|\bjoke|make\s+me\s+laugh)", RegexOptions.IgnoreCase)]
    private static partial Regex Joke();

    [GeneratedRegex(@"(?:ахах|хаха|хехе|\bлол\b|ржу|\bxd\b|\blol\b|lmao|haha|😂|🤣)", RegexOptions.IgnoreCase)]
    private static partial Regex Laugh();

    [GeneratedRegex(@"(?:давай\s+(?:драться|сразимся|биться)|1\s*(?:на|v|vs)\s*1|дуэль|fight\s+me|\bduel\b)", RegexOptions.IgnoreCase)]
    private static partial Regex Fight();

    [GeneratedRegex(@"(?:оружи|пушк|рейлган|рельс|дробовик|базук|\bмеч|railgun|shotgun|bazooka|sword|\bguns?\b|weapon)", RegexOptions.IgnoreCase)]
    private static partial Regex WeaponTalk();

    [GeneratedRegex(@"(?:карт[аеуы]|\bигр[аеуы]|когам|kogama|\bmap\b|\bgame\b)", RegexOptions.IgnoreCase)]
    private static partial Regex Game();

    [GeneratedRegex(@"^\s*(?:да|ага|угу|конечно|ок|окей|точно|yes|yeah|yep|sure|ok|okay)\b", RegexOptions.IgnoreCase)]
    private static partial Regex Yes();

    [GeneratedRegex(@"^\s*(?:нет|не-а|неа|ни за что|no|nope|nah)\b", RegexOptions.IgnoreCase)]
    private static partial Regex No();

    [GeneratedRegex(@"[\p{L}\d-]{4,}")]
    private static partial Regex Word();

    [GeneratedRegex(@"[)]|:\)|:D|\)\)|😊|😄|🙂|😁")]
    private static partial Regex Smile();

    static readonly HashSet<string> Stop = new(StringComparer.OrdinalIgnoreCase)
    {
        "тебя", "меня", "тебе", "мне", "тоже", "есть", "было", "быть", "будет", "может", "можно", "очень",
        "просто", "вообще", "сейчас", "когда", "потом", "почему", "зачем", "какой", "какая", "какие", "который",
        "этот", "эта", "это", "этого", "того", "тут", "там", "здесь", "вот", "ещё", "еще", "уже", "если", "чтобы",
        "только", "давай", "знаешь", "думаю", "хочу", "надо", "нужно", "где", "куда", "откуда", "сколько",
        "that", "this", "what", "with", "have", "from", "your", "about", "just", "really", "there", "they",
        "when", "where", "which", "would", "could", "should", "think", "know", "want", "like", "some", "much",
    };

    // --- ответы -----------------------------------------------------------

    // Игрок написал в чат. text - его сообщение (ник бота в нём можно не
    // убирать, его здесь пропустим).
    public string Reply(string speaker, string text, long now)
    {
        Person p = PersonOf(speaker, now);
        text = text.Trim();
        string bare = Regex.Replace(text, Regex.Escape(Name), " ", RegexOptions.IgnoreCase).Trim(' ', ',', '!', '.');
        // язык - по буквам сообщения; одни смайлики - язык прошлого раза
        if (Regex.IsMatch(bare, @"\p{L}")) p.Russian = Cyrillic().IsMatch(bare);
        bool ru = p.Russian;
        p.Casual = bare.Length < 40 && !(bare.Length > 0 && char.IsUpper(bare[0]));
        p.Smiley = Smile().IsMatch(text);
        bool wordy = bare.Length > 70;
        p.Talks++;
        p.Bye = false;
        p.GreetedAt = now;   // уже говорим - подойдёт снова, здороваться не будет

        string line = Answer(p, bare, ru, now);
        if (wordy && !line.EndsWith('?') && Random.Shared.NextSingle() < 0.5f)
            line += " " + Pick(ru
                ? ["А ты сам как думаешь?", "А что было дальше?", "И как тебе это?"]
                : ["What do you think?", "And then what?", "How do you feel about it?"]);
        return Style(p, line);
    }

    string Answer(Person p, string text, bool ru, long now)
    {
        string you = CallOf(p, ru);

        if (AskMyName().IsMatch(text))
            return p.Called is string called
                ? Pick(ru ? [$"Ты {called}, конечно помню!", $"{called}. Я такое не забываю.", $"Ты же {called}!"]
                          : [$"You're {called}, of course!", $"{called}. I never forget a name.", $"It's {called}!"])
                : Pick(ru ? [$"Ты {you}. А как тебя по-настоящему зовут?", "Ты мне не говорил! Как тебя зовут?"]
                          : [$"You're {you}. What's your real name?", "You never told me! What's your name?"]);

        if (AskMyLikes().IsMatch(text))
            return p.Likes.Count > 0
                ? Pick(ru ? [$"Ты говорил, что любишь {Join(p.Likes, ru)}.", $"Помню: {Join(p.Likes, ru)}!"]
                          : [$"You said you like {Join(p.Likes, ru)}.", $"I remember: {Join(p.Likes, ru)}!"])
                : Pick(ru ? ["Ты ещё не рассказывал. Что ты любишь?", "Не знаю пока. Расскажи!"]
                          : ["You haven't told me yet. What do you like?", "No idea yet. Tell me!"]);

        if (Introduce().Match(text) is { Success: true } intro)
        {
            string called = intro.Groups[1].Value;
            bool again = string.Equals(p.Called, called, StringComparison.OrdinalIgnoreCase);
            p.Called = called;
            Lift(p, 1);
            return again
                ? Pick(ru ? [$"Да помню я, {called}!", $"{called}, ты уже говорил)"] : [$"I know, {called}!", $"You told me already, {called})"])
                : Pick(ru ? [$"Приятно познакомиться, {called}! А я {Name}.", $"{called} - красивое имя. Я {Name}.", $"Запомнил: {called}!"]
                          : [$"Nice to meet you, {called}! I'm {Name}.", $"{called} - cool name. I'm {Name}.", $"Got it: {called}!"]);
        }

        if (Dislike().Match(text) is { Success: true } hate)
        {
            string thing = Clean(hate.Groups[1].Value);
            Add(p.Dislikes, thing);
            p.Likes.RemoveAll(like => string.Equals(like, thing, StringComparison.OrdinalIgnoreCase));
            p.Topic = thing;
            return Pick(ru ? [$"Понимаю, {thing} - не для всех.", $"А что не так с {thing}?", $"Ладно, про {thing} больше ни слова."]
                           : [$"I get it, {thing} isn't for everyone.", $"What's wrong with {thing}?", $"Okay, no more {thing} then."]);
        }

        if (Like().Match(text) is { Success: true } love)
        {
            string thing = Clean(love.Groups[1].Value);
            bool known = p.Likes.Contains(thing, StringComparer.OrdinalIgnoreCase);
            Add(p.Likes, thing);
            p.Topic = thing;
            Lift(p, 1);
            return known
                ? Pick(ru ? [$"Да, ты говорил про {thing}!", $"Я помню, {thing} - твоё)"] : [$"Yeah, you told me about {thing}!", $"I remember, {thing} is your thing)"])
                : Pick(ru ? [$"О, {thing}! Я тоже не против.", $"Запомню, что ты любишь {thing}.", $"{thing}? Расскажи, за что любишь!"]
                          : [$"Oh, {thing}! Not bad at all.", $"I'll remember you like {thing}.", $"{thing}? Tell me why you like it!"]);
        }

        if (Apology().IsMatch(text))
        {
            bool hurt = p.Mood < 0;
            Lift(p, hurt ? 2 : 1);
            return hurt
                ? Pick(ru ? ["Ладно, прощаю.", "Хорошо, мир!", "Ну ладно... забыли."] : ["Fine, I forgive you.", "Okay, peace!", "Alright... forgotten."])
                : Pick(ru ? ["Да всё нормально!", "За что? Всё хорошо)"] : ["It's fine!", "For what? All good)"]);
        }

        if (Insult().IsMatch(text))
        {
            Lift(p, -1);
            return p.Mood <= MoodMin + 1
                ? Pick(ru ? ["Я с тобой больше не разговариваю.", "Ну и грубиян.", "Иди-ка ты... строить."] : ["I'm not talking to you anymore.", "So rude.", "Go... build something."])
                : Pick(ru ? ["Обидно, вообще-то.", "Эй, полегче!", "Сам такой!", "Зачем так грубо?"] : ["That hurts, you know.", "Hey, easy!", "Takes one to know one!", "Why so rude?"]);
        }

        if (Compliment().IsMatch(text))
        {
            Lift(p, 1);
            return Pick(ru ? [$"Спасибо, {you}! Ты тоже ничего)", "Ой, засмущал!", "Приятно слышать!", "Стараюсь!"]
                           : [$"Thanks, {you}! You're not bad either)", "Aw, stop it!", "Nice to hear!", "I try!"]);
        }

        if (Thanks().IsMatch(text))
        {
            Lift(p, 1);
            return Pick(ru ? ["Пожалуйста!", "Обращайся!", "Не за что)"] : ["You're welcome!", "Anytime!", "No problem)"]);
        }

        if (Farewell().IsMatch(text))
        {
            p.Bye = true;
            return Tone(p,
                ru ? [$"Пока, {you}! Заходи ещё.", "Увидимся!", "Было весело, пока!"] : [$"Bye, {you}! Come back soon.", "See you!", "That was fun, bye!"],
                ru ? ["Пока.", "Давай.", "Увидимся."] : ["Bye.", "Later.", "See you."],
                ru ? ["Ну и иди.", "Наконец-то.", "Пока-пока."] : ["Go then.", "Finally.", "Bye-bye."]);
        }

        if (Greeting().IsMatch(text))
        {
            string hi = p.Talks > 3
                ? Tone(p,
                    ru ? [$"О, снова ты, {you}!", $"Привет-привет, {you}! Соскучился?"] : [$"Oh, you again, {you}!", $"Hey hey, {you}! Missed me?"],
                    ru ? [$"Привет ещё раз, {you}.", "Опять здороваемся?"] : [$"Hi again, {you}.", "Hello again?"],
                    ru ? ["Опять ты...", "Ну привет."] : ["You again...", "Well, hi."])
                : Tone(p,
                    ru ? [$"Привет, {you}!", $"Здарова, {you}!", "Привет! Рад тебя видеть."] : [$"Hi, {you}!", $"Hey, {you}!", "Hello! Glad to see you."],
                    ru ? [$"Привет, {you}.", "Привет.", "Здравствуй."] : [$"Hi, {you}.", "Hello.", "Hey."],
                    ru ? ["Чего тебе?", "Ну привет..."] : ["What do you want?", "Well, hi..."]);
            // «привет, как дела?» - и здоровается, и отвечает
            return HowAreYou().IsMatch(text) ? $"{hi} {HowAmI(p, ru)}" : hi;
        }

        if (HowAreYou().IsMatch(text)) return HowAmI(p, ru);

        if (AreYouBot().IsMatch(text))
            return Pick(ru ? ["Я NPC, но поболтать люблю как живой!", "Бот, но с характером)", "А ты как думаешь?)"]
                           : ["I'm an NPC, but I love to chat!", "A bot, but with character)", "What do you think?)"]);

        if (WhoAreYou().IsMatch(text))
            return Pick(ru ? [$"Я {Name}! Местный житель этой карты.", $"Меня зовут {Name}. А тебя?", $"{Name}, приятно познакомиться!"]
                           : [$"I'm {Name}! I live on this map.", $"My name is {Name}. And yours?", $"{Name}, nice to meet you!"]);

        if (Doing().IsMatch(text))
            return Pick(ru ? ["Гуляю тут и болтаю с прохожими.", "С тобой разговариваю!", "Смотрю, кто что строит."]
                           : ["Walking around, chatting with people.", "Talking to you!", "Watching what people build."]);

        if (Joke().IsMatch(text))
            return Pick(ru
                ? ["Почему NPC не играют в прятки? Их всегда видно по нику над головой!",
                   "Купил игрок рейлган. Теперь промахивается с большей дистанции.",
                   "Мой любимый блок - тот, на котором я стою.",
                   "Строитель три часа строил дом. Забыл поставить точку появления."]
                : ["Why don't NPCs play hide and seek? Their nickname gives them away!",
                   "A player bought a railgun. Now he misses from farther away.",
                   "My favourite block is the one I'm standing on.",
                   "A builder spent three hours on a house. Forgot the spawn point."]);

        if (Laugh().IsMatch(text))
            return Pick(ru ? ["Ахах)", "Рад, что смешно!", "Хе-хе"] : ["Haha)", "Glad you liked it!", "Hehe"]);

        if (Fight().IsMatch(text))
            return Pick(ru ? ["Я мирный, драться не буду.", "Не, я за мир. Поищи бойцов на карте.", "Лучше поболтаем!"]
                           : ["I'm peaceful, no fighting.", "Nah, I'm for peace. Find a fighter bot.", "Let's just talk!"]);

        if (WeaponTalk().IsMatch(text))
            return Pick(ru ? ["Рейлган - для терпеливых, дробовик - для смелых.", "Я сам без оружия хожу, мне так спокойнее.", "А у тебя какое любимое оружие?"]
                           : ["Railgun is for the patient, shotgun is for the brave.", "I walk around unarmed, it's calmer.", "What's your favourite weapon?"]);

        if (Game().IsMatch(text))
            return Pick(ru ? ["Мне тут нравится! Кто строил?", "Хорошая карта, я тут каждый угол знаю.", "Тебе как карта?"]
                           : ["I like it here! Who built it?", "Nice map, I know every corner.", "How do you like the map?"]);

        if (Yes().IsMatch(text))
            return Pick(ru ? ["Вот и я так думаю!", "Отлично)", "Договорились."] : ["That's what I think too!", "Great)", "Deal."]);

        if (No().IsMatch(text))
            return Pick(ru ? ["Ну нет так нет.", "Жаль.", "Ладно, как скажешь."] : ["Okay then.", "Too bad.", "Fine, your call."]);

        string? topic = TopicOf(text);
        if (topic is not null) p.Topic = topic;

        if (text.EndsWith('?'))
            return Pick(ru
                ? ["Хм, хороший вопрос. Думаю, да!", "Не знаю... а ты как думаешь?", "Скорее нет, чем да.", topic is null ? "Сложно сказать." : $"Про {topic}? Тут я не мастер, честно."]
                : ["Hmm, good question. I think so!", "Not sure... what do you think?", "Probably not.", topic is null ? "Hard to say." : $"About {topic}? Not my strong side, honestly."]);

        if (p.Likes.Count > 0 && Random.Shared.NextSingle() < 0.2f)
        {
            string like = p.Likes[Random.Shared.Next(p.Likes.Count)];
            return ru ? $"Кстати, ты же любишь {like}. Как с этим?" : $"By the way, you like {like}. How's that going?";
        }
        return topic is not null
            ? Pick(ru ? [$"Про {topic}? Расскажи подробнее!", $"{topic}... интересно. А почему?", $"Никогда не думал про {topic}."]
                      : [$"{topic}? Tell me more!", $"{topic}... interesting. Why?", $"Never thought about {topic}."])
            : Tone(p,
                ru ? ["Ага, понимаю!", "Интересно)", "Продолжай!"] : ["I see!", "Interesting)", "Go on!"],
                ru ? ["Понятно.", "Ага.", "Ну да."] : ["I see.", "Okay.", "Right."],
                ru ? ["И?", "Ну-ну.", "Мне всё равно."] : ["So?", "Sure.", "Whatever."]);
    }

    // «Как дела?» - по настроению к этому игроку.
    string HowAmI(Person p, bool ru) => Tone(p,
        ru ? ["Отлично! Гуляю, болтаю. А у тебя как?", "Всё супер! А ты как?", "Хорошо, особенно когда ты рядом)"]
           : ["Great! Walking around, chatting. And you?", "All good! How about you?", "Good, especially with you around)"],
        ru ? ["Нормально. А у тебя?", "Да потихоньку. Ты как?", "Как обычно. А ты?"] : ["Fine. You?", "Same as usual. And you?", "Okay. How about you?"],
        ru ? [p.Kills > 0 ? "Было бы лучше, если б в меня не стреляли." : "Бывало и лучше.", "Так себе."]
           : [p.Kills > 0 ? "Would be better if people stopped shooting me." : "Been better.", "Meh."]);

    // Игрок только что попрощался.
    public bool SaidBye(string speaker) => _people.TryGetValue(speaker, out Person? p) && p.Bye;

    // Игрок подошёл: бот здоровается первым, если давно не виделись.
    public string? Greet(string speaker, long now)
    {
        Person p = PersonOf(speaker, now);
        if (now - p.GreetedAt < GreetEveryMs) return null;
        p.GreetedAt = now;
        bool ru = p.Russian;
        string you = CallOf(p, ru);
        return Style(p, p.Talks == 0
            ? Pick(ru ? [$"Привет, {you}! Я {Name}. Поболтаем?", $"О, новенький! Привет, {you}!", "Привет! Напиши мне что-нибудь."]
                      : [$"Hi, {you}! I'm {Name}. Wanna chat?", $"Oh, a new face! Hi, {you}!", "Hi! Write me something."])
            : Tone(p,
                ru ? [$"{you}, ты вернулся!", $"Привет снова, {you}!"] : [$"{you}, you're back!", $"Hi again, {you}!"],
                ru ? [$"А, это ты, {you}.", "Снова привет."] : [$"Oh, it's you, {you}.", "Hello again."],
                ru ? ["Только не стреляй опять.", "Опять ты..."] : ["Just don't shoot me again.", "You again..."]));
    }

    // В бота попал этот игрок.
    public string? Hurt(string speaker, long now)
    {
        Person p = PersonOf(speaker, now);
        if (now - p.HurtAt < HurtEveryMs) return null;
        p.HurtAt = now;
        Lift(p, -1);
        bool ru = p.Russian;
        return Style(p, p.Mood <= MoodMin + 1
            ? Pick(ru ? ["Да хватит уже!", "Опять?! Я же мирный!", "Перестань!"] : ["Stop it already!", "Again?! I'm peaceful!", "Quit it!"])
            : Pick(ru ? ["Эй, за что?!", "Ай! Я же ничего не сделал.", "Больно вообще-то!", "Ты чего?!"]
                      : ["Hey, what was that for?!", "Ouch! I did nothing.", "That hurts!", "What are you doing?!"]));
    }

    public void Killed(string speaker, long now)
    {
        Person p = PersonOf(speaker, now);
        p.Kills++;
        Lift(p, -2);
    }

    // Возродился после того, как его убил этот игрок.
    public string Respawned(string killer, long now)
    {
        Person p = PersonOf(killer, now);
        bool ru = p.Russian;
        string you = CallOf(p, ru);
        return Style(p, p.Kills > 2
            ? Pick(ru ? [$"{you}, это уже {p.Kills}-й раз!", "Я веду счёт, между прочим.", "Тебе не надоело?"]
                      : [$"{you}, that's {p.Kills} times now!", "I'm keeping count, you know.", "Aren't you bored?"])
            : Pick(ru ? ["Я вернулся! И я всё помню.", $"Ну спасибо, {you}...", "Ну вот, опять возрождаться."]
                      : ["I'm back! And I remember everything.", $"Thanks a lot, {you}...", "Great, respawning again."]));
    }

    // --- помощники --------------------------------------------------------

    Person PersonOf(string speaker, long now)
    {
        if (!_people.TryGetValue(speaker, out Person? p)) _people[speaker] = p = new Person(speaker) { MoodAt = now };
        // обида со временем проходит, восторг - тоже
        while (p.Mood != 0 && now - p.MoodAt >= MoodHealMs)
        {
            p.Mood -= Math.Sign(p.Mood);
            p.MoodAt += MoodHealMs;
        }
        if (p.Mood == 0) p.MoodAt = now;
        return p;
    }

    static void Lift(Person p, int by) => p.Mood = Math.Clamp(p.Mood + by, MoodMin, MoodMax);

    // Как обращаться: по имени, которое назвал, иначе по нику.
    static string CallOf(Person p, bool ru) =>
        p.Called ?? (p.Nick.Length > 0 ? p.Nick : ru ? "друг" : "friend");

    // Фраза по настроению: тёплая, обычная или ворчливая.
    string Tone(Person p, string[] warm, string[] plain, string[] grumpy) =>
        Pick(p.Mood >= 2 ? warm : p.Mood <= -2 ? grumpy : plain);

    string Pick(string[] options)
    {
        string[] fresh = [.. options.Where(option => !_recent.Contains(option))];
        string line = (fresh.Length > 0 ? fresh : options)[Random.Shared.Next(fresh.Length > 0 ? fresh.Length : options.Length)];
        _recent.Enqueue(line);
        while (_recent.Count > Remember) _recent.Dequeue();
        return line;
    }

    // В манере собеседника: пишет строчными и коротко - и бот так же; со
    // смайликами - и бот улыбается, если не сердит.
    string Style(Person p, string line)
    {
        // имена строчными не пишем
        bool named = new[] { p.Called, p.Nick, Name }.Any(word => !string.IsNullOrEmpty(word) && line.StartsWith(word, StringComparison.Ordinal));
        if (p.Casual && line.Length > 1 && !named)
        {
            line = char.ToLowerInvariant(line[0]) + line[1..];
            if (line.EndsWith('.') && !line.EndsWith("..")) line = line[..^1];
        }
        if (p.Smiley && p.Mood >= 0 && !line.EndsWith(')') && Random.Shared.NextSingle() < 0.6f) line += ")";
        return line.Length > MaxLength ? line[..MaxLength] : line;
    }

    static string Clean(string thing)
    {
        thing = thing.Trim().TrimEnd('.', '!', '?', ',', ')', '(');
        int cut = thing.IndexOfAny(['.', '!', '?', ',']);
        return cut > 1 ? thing[..cut].Trim() : thing;
    }

    static void Add(List<string> list, string thing)
    {
        if (thing.Length < 2 || list.Contains(thing, StringComparer.OrdinalIgnoreCase)) return;
        list.Add(thing);
        if (list.Count > 5) list.RemoveAt(0);
    }

    static string Join(List<string> list, bool ru) =>
        list.Count == 1 ? list[0] : string.Join(", ", list.Take(list.Count - 1)) + (ru ? " и " : " and ") + list[^1];

    static string? TopicOf(string text) =>
        Word().Matches(text).Select(match => match.Value).Where(word => !Stop.Contains(word)).MaxBy(word => word.Length);
}
