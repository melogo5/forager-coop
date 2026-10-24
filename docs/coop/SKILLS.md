# Дерево навыков Forager (текущее)

## Кооп: одно дерево на всех (с 2026-10-24)

Решение пользователя: все навыки общие, деления на общие и раздельные нет. Изучил навык один игрок — он изучен у
всех: виден изученным в каждом дереве и действует у всех (`native/src/skills.cpp`).

- Дерево — `global.skillUnlocked` хоста (хранится в сейве мира); гость получает его вместе с миром.
- Купивший навык шлёт дерево и вызовы `ItemEdit`/`StructureEdit`, сделанные покупкой (objSkill Step_0: что навык
  открывает); остальные объединяют деревья (по каждому навыку — большее из «закрыт / доступен / изучен») и повторяют
  эти вызовы. Хост передаёт дерево гостя остальным гостям.
- Очки навыков у каждого свои: очко за каждое повышение общего уровня, минус купленное этим игроком. Поэтому вдвоём
  дерево заполняется примерно вдвое быстрее — см. TODO в QUESTIONS.md (урезать опыт).
- Навыки, изученные гостем раньше в личном дереве (до 2026-10-24), при его входе добавляются в общее дерево; то, что
  они тогда открыли у гостя, не повторяется.

Источник: `local/russian.json`, `local/english.json` (ключи `skillName<N>`, `skillDesc<N>`), раскладка — `objSkillGridControl` Create_0 (`grid[x, y] = skill`, сетка 8×8). Ветки: 0–15 промышленность, 16–31 экономика, 32–47 собирательство, 48–63 магия; каждая — квадрант 4×4, первые навыки веток (0, 16, 32, 48) сходятся в центре.

## Сетка

```
 15  14  13  12  28  29  30  31
 11  10   9   8  24  25  26  27
  7   6   5   4  20  21  22  23
  3   2   1   0  16  17  18  19
 35  34  33  32  48  49  50  51
 39  38  37  36  52  53  54  55
 43  42  41  40  56  57  58  59
 47  46  45  44  60  61  62  63
```

| y \ x | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| 0 | 15 лазеры | 14 инженерия | 13 физика | 12 ювелирное дело | 28 припасы | 29 искусная работа | 30 колонизация | 31 логистика |
| 1 | 11 автоматизация | 10 железные дороги | 9 выплавка | 8 текстиль | 24 сделки | 25 капитализм | 26 сокровища | 27 авторемонт |
| 2 | 7 бурение | 6 производство | 5 мастерство | 4 вышивание | 20 торговля | 21 банковское дело | 22 управленец | 23 архитектура |
| 3 | 3 оборудование | 2 каменная кладка | 1 плотничное дело | 0 промышленность | 16 экономика | 17 хранение | 18 азартные игры | 19 оптика |
| 4 | 35 питомец | 34 фермерство | 33 собирательство | 32 сбор пищи | 48 магия | 49 алхимия | 50 магические письмена | 51 чародейство |
| 5 | 39 геология | 38 горное дело | 37 рыбная ловля | 36 охота | 52 бой | 53 уворот | 54 реагенты | 55 вера |
| 6 | 43 старательство | 42 гурман | 41 кулинария | 40 баллистика | 56 второе дыхание | 57 колдовство | 58 трансмутация | 59 колдовство |
| 7 | 47 залежи | 46 смекалка | 45 обжорство | 44 сбор добычи | 60 разрушение | 61 духовность | 62 призыв | 63 астрология |

## Промышленность

| ID | Название | Описание | English |
|---|---|---|---|
| 0 | промышленность | открывает сталь; открывает стекло; дает опыт за возведение построек | industry — unlocks steel; unlocks glass; gain EP when building structures |
| 1 | плотничное дело | открывает плотничные мастерские; постройкам требуется на 25% меньше древесины | carpentry — unlocks carpentry stations; structures cost 25% less wood |
| 2 | каменная кладка | открывает мастерские каменщиков; открывает жаровни; постройкам требуется на 25% меньше камня и кирпичей | masonry — unlocks masonry tables; unlocks braziers; structures cost 25% less stone and brick |
| 3 | оборудование | открывает пресс для цветов; у всех построек есть шанс 10% создать вдвое больше предметов | machinery — unlocks flower press; all structures have a 10% chance to craft double items |
| 4 | вышивание | открывает кожу; швейные мастерские работают на 25% быстрее | sewing — unlocks leather; sewing stations work 25% faster |
| 5 | мастерство | открывает молот; открывает королевскую сталь; открывает королевскую одежду | craftsmanship — unlocks hammer; unlocks royal steel; unlocks royal clothing |
| 6 | производство | открывает фабрики | manufacturing — unlocks factories |
| 7 | бурение | открывает буровые установки; буровые установки могут бесплатно собирать песок; дает шанс 25% найти уголь во время копания | drilling — unlocks offshore drills; offshore drills may gather sand for free; provides a 25% chance to find coal when digging |
| 8 | текстиль | открывает ботинки; открывает перчатки | textiles — unlocks boots; unlocks gloves |
| 9 | выплавка | печи работают на 25% быстрее; кузницы работают на 25% быстрее | smelting — furnaces work 25% faster; forges work 25% faster |
| 10 | железные дороги | открывает паровозы; открывает железные дороги | railroads — unlocks locomotives; unlocks railroads |
| 11 | автоматизация | открывает жезлы горняков; созданные предметы собираются автоматически | automation — unlocks mining rods; crafted items collect themselves |
| 12 | ювелирное дело | открывает амулеты; продажа самоцветов дает на 20% больше монет | jewelry — unlocks amulets; gems sell for 20% more coins |
| 13 | физика | открывает электростанции | physics — unlocks power plants |
| 14 | инженерия | все промышленные постройки работают на 25% быстрее; открывает дроидов; открывает эми-гранаты | engineering — industrial structures work 25% faster; unlocks droids; unlocks emp grenades |
| 15 | лазеры | жезлы горняков наносят удвоенный урон; дроиды наносят удвоенный урон | lasers — mining rods deal double damage; droids deal double damage |

## Экономика

| ID | Название | Описание | English |
|---|---|---|---|
| 16 | экономика | немедленно дает 40 монет; кузницы всегда создают на 4 монеты больше | economy — instantly gain 40 coins; forges create 4 extra  coins every time |
| 17 | хранение | открывает хранилища | storage — unlocks vaults |
| 18 | азартные игры | открывает игровые автоматы | gambling — unlocks slot machines |
| 19 | оптика | открывает маяки; удваивает радиус света в темноте | optics — unlocks lighthouses; doubles light radius in the dark |
| 20 | торговля | открывает рынки | trade — unlocks markets |
| 21 | банковское дело | открывает банки | banking — unlocks banks |
| 22 | управленец | предметы можно отправлять в хранилища прямо из снаряжения; у хранилищ могут быть закрытые ячейки предметов | management — items may be sent to vaults from the inventory; vaults may have locked item slots |
| 23 | архитектура | удвоенная прочность построек; постройки стоят на 25% меньше материалов; больше ячеек в хранилищах | architecture — structures have double health; structures cost 25% fewer materials; vaults have more inventory slots |
| 24 | сделки | снижает рыночные цены на 15% | bargain — reduces market prices by 15% |
| 25 | капитализм | дает опыт за продажу предметов; выброшенные предметы немедленно; отправляются на продажу | capitalism — gain EP when selling items; items discarded from your  inventory are instantly  sold instead |
| 26 | сокровища | банки приносят монеты на 50% быстрее,; если рядом есть другие банки | treasury — banks generate coins 50% faster  when adjacent to other banks |
| 27 | авторемонт | постройки ремонтируются автоматически | autorepair — structures automatically  repair themselves |
| 28 | припасы | на рынках продается больше предметов; рынки пополняют запас предметов на 50% быстрее | supply — markets sell more items; market restock time is reduced by 50% |
| 29 | искусная работа | продажа созданных предметов дает на 25% больше монет | artistry — crafted items sell  for 25% more coins |
| 30 | колонизация | покупка земли на 30% дешевле | colonization — purchasing lands is 30% cheaper |
| 31 | логистика | у маяков удвоен радиус действия; у жезлов горняков удвоен радиус действия; у электростанций удвоен радиус действия; у баллист удвоен радиус действия | logistics — lighthouses have double area of effect; mining rods have double area of effect; power plants have double area of effect; ballistas have double area of effect |

## Собирательство

| ID | Название | Описание | English |
|---|---|---|---|
| 32 | сбор пищи | хлопок появляется чаще; позволяет находить пшеницу; позволяет находить свеклу | foraging — cotton spawns more often; allows you to find wheat; allows you to find beets |
| 33 | собирательство | дает 4 новые ячейки снаряжения; дает на 25% больше опыта за растения и урожай | gathering — gain 4 inventory slots; gain 25% more EP from plants and crops |
| 34 | фермерство | открывает ветряные мельницы; открывает опрыскиватели | farming — unlocks windmills; unlocks sprinklers |
| 35 | питомец | создает дружественного спутника | pet — spawns a friendly companion |
| 36 | охота | открывает луки; открывает стрелы; убитые враги оставляют после себя монеты | hunting — unlocks bows; unlocks arrows; enemies drop coins when killed |
| 37 | рыбная ловля | открывает удочки; ловушки для рыбы действуют на 50% быстрее | fishing — unlocks fishing rods; fishing traps catch things 50% faster |
| 38 | горное дело | камни оставляют после себя на 40% больше минералов | mining — rocks drop 40% more minerals |
| 39 | геология | все камни оставляют после себя уголь | geology — all rocks drop coal |
| 40 | баллистика | открывает баллисты; луки выпускают по 3 стрелы за раз | ballistics — unlocks ballistas; bows shoot triple arrows |
| 41 | кулинария | открывает кастрюли; открывает сахар; открывает майонез; открывает сыр | cooking — unlocks cookpots; unlocks sugar; unlocks mayo; unlocks cheese |
| 42 | гурман | позволяет поедать минералы и самоцветы | calciverous — allows you to eat  minerals and gems |
| 43 | старательство | камни на 10% чаще оставляют после себя самоцветы; на 30% повышает шанс найти редкие предметы во время копания | prospecting — rocks have 10% more chances of dropping gems; increases the chances of finding rare items when digging by 30% |
| 44 | сбор добычи | дает на 50% больше добычи за врагов и боссов | looting — gain 50% more loot  from enemies and bosses |
| 45 | обжорство | дает на 20% больше энергии во время еды; дает опыт за прием пищи | gluttony — gain 20% more energy  when eating; gain EP when eating |
| 46 | смекалка | все постройки для фермы работают на 25% быстрее | prowess — farming structures work 25% faster |
| 47 | залежи | открывает каменоломни | deposit — unlocks quarries |

## Магия

| ID | Название | Описание | English |
|---|---|---|---|
| 48 | магия | дает на 25% больше опыта | magic — gain 25% more EP |
| 49 | алхимия | открывает котелки | alchemy — unlocks cauldrons |
| 50 | магические письмена | открывает таблички магии | inscription — unlocks inscription tables |
| 51 | чародейство | открывает свитки и зелья эксперта | thaumaturgy — unlocks expert level  scrolls and potions |
| 52 | бой | открывает мечи; навсегда увеличивает базовый урон на 1 | combat — unlocks swords; increases base damage by 1 |
| 53 | уворот | повышает шанс уклонения на 15% | evasion — increases dodge chance by 15% |
| 54 | реагенты | на создание зелий и свитков требуется меньше ресурсов | reagency — potions and scrolls cost  fewer resources to craft |
| 55 | вера | открывает алтари | faith — unlocks shrines |
| 56 | второе дыхание | восстанавливает энергию за убийство врагов | renewal — recover energy when  killing enemies |
| 57 | колдовство | усиления действуют на 50% дольше; дает опыт за использование зелий; дает опыт за использование свитков | spellbind — buffs last 50% longer; gain EP when drinking potions; gain EP when using scrolls |
| 58 | трансмутация | открывает сталь пустоты; позволяет котелкам превращать сталь в самоцветы | transmutation — unlocks void steel; allows cauldrons to  morph steel into gems |
| 59 | колдовство | магические постройки работают на 25% быстрее | conjuration — magic structures work 25% faster |
| 60 | разрушение | наносит боссам на 25% больше урона | destruction — deal 25% increased damage to bosses |
| 61 | духовность | дает 2 сферы духа; открывает кузницы духа | spirituality — gain 2 spirit orbs; unlocks spirit forges |
| 62 | призыв | открывает мастерскую знаков; алтари заряжаются вдвое быстрее | summoning — unlocks sigil maker; shrines recharge twice as fast |
| 63 | астрология | открывает космическую сталь; вероятность звездопада увеличивается | astrology — unlocks cosmic steel; starfalls happen more frequently |
