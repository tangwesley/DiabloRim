#!/usr/bin/perl
# Generates DiabloLoot_affixes.csv from the balance numbers in
# "Skyrim attributes (1).xlsx". Regenerate rather than hand-editing structure;
# hand-edit the tuning columns (weight, minItemLevel, mgef, prefix, suffix).
use strict; use warnings;

# name | category | slots | t1lo t1hi | t2lo t2hi | t3lo t3hi | unit | duration | area | note
my @tiered = (
 ["Armor","Physical","ARMOR|SHIELD",5,10,11,20,21,35,"flat",0,0],
 # ★FLAT DAMAGE IS CUT TOO, for the same reason and one more.
 # A weapon enchantment is fire-and-forget on hit; Skyrim has no constant
 # "+2 damage" weapon enchantment. The nearest actor value, AttackDamageMult,
 # is a MULTIPLIER and would have to ride apparel -- where it duplicates the
 # One-Handed / Two-Handed / Archery affixes that already exist. The elemental
 # damage and absorb affixes cover the offensive slot without it.
 # ★ATTACK SPEED and CRITICAL DAMAGE ARE CUT FOR MVP.
 # Both were moved off weapons because neither is expressible as a weapon
 # on-hit enchantment. The survey then showed why they are worse than that:
 # of 5039 magic effects in a 995-mod load order, NOTHING uses WeaponSpeedMult
 # or CriticalChance in an enchantment -- only perks and Boons. Both would need
 # authoring AND verification that a perk-shaped effect works as constant-effect
 # apparel. Critical Damage is worse again: Skyrim has no crit-damage actor
 # value at all, only crit CHANCE, so the affix cannot mean what the sheet says.
 # Restore them by re-adding the rows; nothing else depends on their absence.
 ["Health","Attribute","ARMOR|SHIELD",10,20,21,40,41,60,"flat",0,0],
 ["Magicka","Attribute","ARMOR|RING|AMULET",10,20,21,40,41,60,"flat",0,0],
 ["Stamina","Attribute","ARMOR|RING|AMULET",10,20,21,40,41,60,"flat",0,0],
 ["Carry Weight","Attribute","ARMOR|RING|AMULET",10,20,21,40,41,60,"flat",0,0],
 ["Health Regen","Regeneration","ARMOR|RING|AMULET",10,20,21,35,36,50,"pct",0,0],
 ["Magicka Regen","Regeneration","ARMOR|RING|AMULET",20,40,41,70,71,100,"pct",0,0],
 ["Stamina Regen","Regeneration","ARMOR|RING|AMULET",10,20,21,35,36,50,"pct",0,0],
 # ★WEAPON SKILLS ROLL ON THE WEAPON THEIR SKILL GOVERNS, never another.
 # The slots column knows three typed weapon tokens beside WEAPON:
 #   ONEHANDED  sword, dagger, axe, mace
 #   TWOHANDED  greatsword, battleaxe, warhammer
 #   BOW        bow and crossbow
 #   STAFF      staff -- no skill, but one hand, with a spell in the other
 # A weapon presents WEAPON plus its type, so a WEAPON row reaches every
 # weapon and a TWOHANDED row reaches only greatswords. The plugin ALSO
 # narrows on load: any row whose effect fortifies One-Handed, Two-Handed or
 # Archery and lists WEAPON (or the wrong type) is cut to the matching type,
 # so a two-handed weapon cannot carry a One-Handed bonus however the CSV is
 # written. One-Handed narrows to ONEHANDED|STAFF: a staff is held in one
 # hand, and the decision was that the bonus belongs on it.
 # ★ON A WEAPON THE BUFF IS DELIVERED BY THE PLUGIN, NOT THE ENCHANTMENT. A
 # weapon enchantment reaches only what it hits, so Wielder.cpp hands the
 # holder an ability for the length of the equip. The effect stays in the
 # enchantment as well, so the card reads right; on hit it does nothing.
 ["One-Handed","Combat Skill","ONEHANDED|STAFF|ARMOR|RING|AMULET",5,10,11,20,21,30,"pct",0,0],
 ["Two-Handed","Combat Skill","TWOHANDED|ARMOR|RING|AMULET",5,10,11,20,21,30,"pct",0,0],
 ["Archery","Combat Skill","BOW|ARMOR|RING|AMULET",5,10,11,20,21,30,"pct",0,0],
 ["Block","Combat Skill","SHIELD|ARMOR",5,10,11,20,21,30,"pct",0,0],
 ["Heavy Armor","Armor Skill","ARMOR|RING|AMULET",5,10,11,18,19,25,"flat",0,0],
 ["Light Armor","Armor Skill","ARMOR|RING|AMULET",5,10,11,18,19,25,"flat",0,0],
 ["Sneak","Stealth","ARMOR|RING|AMULET",10,15,16,25,26,40,"pct",0,0],
 ["Lockpicking","Stealth","ARMOR|RING|AMULET",10,15,16,25,26,40,"pct",0,0],
 ["Pickpocket","Stealth","ARMOR|RING|AMULET",10,15,16,25,26,40,"pct",0,0],
 ["Barter","Economy","HEAD|AMULET|RING",5,10,11,17,18,25,"pct",0,0],
 ["Alchemy","Crafting","ARMOR|RING|AMULET",5,8,9,15,16,20,"pct",0,0],
 ["Smithing","Crafting","ARMOR|RING|AMULET",5,8,9,15,16,20,"pct",0,0],
 ["Magic Resist","Resistance","ARMOR|SHIELD|RING|AMULET",5,8,9,14,15,20,"pct",0,0],
 ["Poison Resist","Resistance","ARMOR|RING|AMULET",15,30,31,60,61,100,"pct",0,0],
 ["Disease Resist","Resistance","ARMOR|RING|AMULET",15,30,31,60,61,100,"pct",0,0],
 ["Fire Damage","Weapon Damage","WEAPON",3,7,8,15,16,25,"flat",0,0],
 ["Frost Damage","Weapon Damage","WEAPON",3,7,8,15,16,25,"flat",0,0],
 ["Shock Damage","Weapon Damage","WEAPON",3,7,8,15,16,25,"flat",1,0],
 ["Damage Magicka","Weapon Damage","WEAPON",10,20,21,40,41,60,"flat",0,0],
 ["Damage Stamina","Weapon Damage","WEAPON",5,10,11,20,21,30,"flat",0,0],
 ["Absorb Health","Absorb","WEAPON",3,7,8,15,16,25,"flat",1,0],
 ["Absorb Magicka","Absorb","WEAPON",5,10,11,20,21,30,"flat",1,0],
 ["Absorb Stamina","Absorb","WEAPON",5,10,11,20,21,30,"flat",1,0],
 # ★EVERY DURATION BELOW IS MEASURED, not chosen. The survey reports the MODE
 # across every vanilla enchantment that uses each effect. Of ten affixes with
 # a non-trivial timing, six of my guesses were wrong:
 #
 #   absorb_health/magicka/stamina  0 -> 1   (unanimous in vanilla)
 #   shock_damage                   0 -> 1   (23 usages at 1, 3 at 0)
 #   soul_trap                      4 -> 3   (the sheet said "3-5 sec"; vanilla says 3)
 #   banish                        30 -> 0   (Banish is INSTANT, not a 30s effect)
 #
 # and four were right: fear 30, turn_undead 30, fire_damage 0, frost_damage 0.
 # Note fire and frost really do use 0 while shock uses 1 -- the asymmetry is
 # Bethesda's, not a measurement error.
 #
 # ★PARALYSIS IS THE ONE PLACE THE EVIDENCE IS REJECTED. Vanilla ships it at
 # seven different durations (4,5,6,8,10,12,14) with no consensus, and our
 # design encodes the TIER as the duration -- 1 sec = +2, 2 sec = +3, straight
 # from the sheet. Taking vanilla's mode of 10 would flatten the ladder and make
 # the affix wildly stronger than its point cost.
 # ★Control effects are DURATION-based. Phase 0 measured what duration=0 costs:
 # Turn Undead rendered as "flee for 0 seconds" and did nothing at all.
 ["Fear","Control","WEAPON",1,7,8,14,15,20,"level",30,0],
 ["Turn Undead","Control","WEAPON",1,12,13,25,26,40,"level",30,0],
 ["Banish","Control","WEAPON",1,12,13,24,25,36,"level",0,0],
 ["Unarmed Damage","Unarmed","HANDS|RING",3,5,6,10,11,15,"flat",0,0],
);

# Spell Cost expands per school; Element Resist per element.
#
# *POSITIVE VALUES, and the sheet is why this was wrong once. The sheet writes
# "-5-8%" meaning "reduce cost by 5-8 percent", but Skyrim Fortify <School>
# already takes a POSITIVE magnitude meaning "costs N percent less". Encoding
# the minus literally produced mag=-5.2, which the engine CLAMPED TO ZERO --
# measured on a necklace reading "Conjuration spells cost 0% less". A silently
# dead affix, the same failure mode as duration=0.
# ★SPELL COST ROLLS ON ONE-HANDED WEAPONS AND STAVES as well as apparel: a
# sword or a staff in one hand leaves the other free for the spell, and the
# plugin delivers the reduction to the wielder the same way it does a weapon
# skill (Wielder.cpp). Two-handed weapons and bows fill both hands, so not
# those.
my @schools = qw(Alteration Conjuration Destruction Illusion Restoration);
for my $s (@schools) {
  push @tiered, ["$s Cost","Magic","ONEHANDED|STAFF|ARMOR|RING|AMULET",5,8,9,15,16,20,"pct",0,0];
}
for my $e (qw(Fire Frost Shock)) {
  push @tiered, ["$e Resist","Resistance","ARMOR|SHIELD|RING|AMULET",10,20,21,35,36,50,"pct",0,0];
}

# Toggles: fixed point cost, no tier ladder.
my @toggles = (
 ["Waterbreathing","Toggle","HEAD|RING|AMULET",1],
 ["Soul Trap","Toggle","WEAPON",1],
 ["Disease Immunity","Toggle","ARMOR|RING|AMULET",1],
 ["Muffle","Toggle","FEET",2],
 ["Poison Immunity","Toggle","ARMOR|RING|AMULET",2],
);

# Default gates. Tier 1 from the start, tier 2 mid-game, tier 3 late.
my @minLevel = (0, 1, 12, 25);

# The mgef column is filled from the map the in-game survey emits, so the
# FormIDs come from what the engine actually resolved rather than from anyone
# transcribing them. Tokens are "plugin|0xLOCALID" -- never raw FormIDs, whose
# high byte encodes load-order position and differs per machine.
my %MGEF;
if (open my $m, "<", "DiabloLoot_mgef_map.csv") {
  while (<$m>) {
    next if /^#/ or /^s*$/;
    chomp;
    my ($id, $tok) = split /,/;
    $MGEF{$id} = $tok if defined $tok and $tok ne "";
  }
  close $m;
}
sub mgef { my $id = shift; return $MGEF{$id} // ""; }

# Name fragments, same pattern as the mgef map: a separate file, so regenerating
# the table never discards hand-authored flavour.
my (%PRE, %SUF);
if (open my $n, "<", "DiabloLoot_names.csv") {
  while (<$n>) {
    next if /^#/ or /^\s*$/;
    chomp;
    my ($id, $p, $sfx) = split /,/;
    next unless defined $id;
    $PRE{$id} = defined $p ? $p : "";
    $SUF{$id} = defined $sfx ? $sfx : "";
  }
  close $n;
}
sub pre { my $id = shift; return defined $PRE{$id} ? $PRE{$id} : ""; }
sub suf { my $id = shift; return defined $SUF{$id} ? $SUF{$id} : ""; }

sub id { my $n = lc shift; $n =~ s/[^a-z0-9]+/_/g; $n =~ s/^_|_$//g; return $n; }

print "affixId,name,category,tier,points,minValue,maxValue,unit,duration,area,slots,weight,minItemLevel,mgef,npcExclude,prefix,suffix\n";

for my $a (@tiered) {
  my ($name,$cat,$slots,$t1lo,$t1hi,$t2lo,$t2hi,$t3lo,$t3hi,$unit,$dur,$area) = @$a;
  my @b = ([$t1lo,$t1hi],[$t2lo,$t2hi],[$t3lo,$t3hi]);
  for my $t (1..3) {
    # ★Paralysis is the sheet's one explicit exception: no Tier I, 1 sec = +2,
    # 2 sec = +3. Handled below as its own entry, not here.
    printf "%s,%s,%s,%d,%d,%d,%d,%s,%d,%d,%s,%d,%d,%s,0,%s,%s
",
      id($name),$name,$cat,$t,$t,$b[$t-1][0],$b[$t-1][1],$unit,$dur,$area,$slots,100,$minLevel[$t],mgef(id($name)),pre(id($name)),suf(id($name));
  }
}

# Paralysis: tiers 2 and 3 only, duration in seconds.
printf "paralysis,Paralysis,Control,%d,%d,%d,%d,seconds,%d,0,WEAPON,%d,%d,%s,0,%s,%s
",
  2,2,1,1,1,35,$minLevel[2],mgef("paralysis"),pre("paralysis"),suf("paralysis");
printf "paralysis,Paralysis,Control,%d,%d,%d,%d,seconds,%d,0,WEAPON,%d,%d,%s,0,%s,%s
",
  3,3,2,2,2,35,$minLevel[3],mgef("paralysis"),pre("paralysis"),suf("paralysis");

for my $t (@toggles) {
  my ($name,$cat,$slots,$pts) = @$t;
  printf "%s,%s,%s,1,%d,1,1,toggle,%d,0,%s,%d,%d,%s,0,%s,%s
",
    id($name),$name,$cat,$pts,($name eq 'Soul Trap' ? 3 : 0),$slots,100,$minLevel[$pts],mgef(id($name)),pre(id($name)),suf(id($name));
}
