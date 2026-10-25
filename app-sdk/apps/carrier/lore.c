/* Original CARRIER words. Packed banks avoid pointer/relocation tables; mutable
 * output stays in the existing PSRAM game pool. Nothing here draws or does IO. */
#include "internal.h"

static const char *part(const char *bank, unsigned n) {
    while(n--) bank+=strlen(bank)+1;
    return bank;
}
static const char known[] =
    "Salt kites\0Glass eels\0Copper tea\0Ice regattas\0Night orchards\0"
    "Thunder bread\0Singing dunes\0Relay ruins\0Pearl foundries\0Fog wrestling\0"
    "Lantern whales\0Ribbon storms\0Blue cider\0Tidal libraries\0Cloud racing\0"
    "Coral engines\0Signal choirs\0Ash gardens\0Spiral shells\0Rain festivals\0"
    "Floating bakeries\0Basalt harbours\0Mirror foxes\0Moss silk\0Storm ferries\0"
    "Amber reefs\0Tin flutes\0Spore lanterns\0Kelp noodles\0Whisper mines\0"
    "Dawn markets\0Drum glaciers\0Vacuum pottery\0Wreck museums\0Red snow\0"
    "Paper submarines\0Cinder honey\0Meteor bowls\0Lichen towers\0Rooftop tides";
/* Five shades per government; economy supplies the working landscape. */
static const char mood[] =
    "unruled\0watchful\0bartered\0restless\0unclaimed\0"
    "clan-run\0oathbound\0feuding\0kin-held\0hereditary\0"
    "partitioned\0negotiated\0district-run\0fractious\0disputed\0"
    "patrolled\0rationed\0barracked\0guarded\0drilled\0"
    "communal\0shared\0collective\0cooperative\0union-run\0"
    "federated\0allied\0council-run\0chartered\0pactbound\0"
    "debating\0petitioning\0voting\0elected\0consensus-led\0"
    "licensed\0audited\0contracted\0bonded\0franchised";
static const char quirk[] =
    "Bells mark rain\0Doors face dawn\0Tea buys a name\0Kites carry mail\0No clocks at noon\0"
    "Debts pass to kin\0Shoes stay outside\0Silence means yes\0Maps omit wells\0Bread seals vows\0"
    "Songs record rent\0Pets inherit boats\0Names change yearly\0Hats show trades\0Coins have holes\0"
    "Bridges have names\0Dusk is teatime\0Guests plant seeds\0Mirrors face down\0Crews share cups\0"
    "Rain starts races\0Wakes last a week\0Votes use shells\0Lifts play drums\0Locks use whistles\0"
    "Salt marks borders\0Moths predict wind\0Letters need rhyme\0No bells at night\0Lunch stops traffic\0"
    "Children name ships\0Courts meet afloat\0Roofs grow herbs\0Tides set paydays\0All ferries are red\0"
    "Pilots trade seeds\0Frost names streets\0Tools are heirlooms\0Poets count storms\0Receipts are sung";
void cg_description(game_system *s) {
    uint32_t r=cg_hash(s->seed ^ 0x4c4f5245u);
    const char *work=s->economy<2?"shipyards":s->economy<4?"mills":s->economy<6?"orchards":"farms";
    snprintf(s->description,sizeof(s->description),"%.9s: %s; %s %s.%s%s%s",
        s->name,part(known,r%40),part(mood,(s->government%8)*5+(cg_hash(r)%5)),work,
        (r&3)?" ":"",(r&3)?part(quirk,cg_hash(r^s->economy)%40):"",(r&3)?".":"");
}
void cg_station_name(uint32_t seed,char out[20]) {
    static const char roots[]="Hadley\0Vane\0Nacre\0Brindle\0Tern\0Sable\0Harrow\0Morrow\0"
        "Orison\0Tallow\0Rill\0Fathom\0Kest\0Skein\0Loom\0Cairn\0Aster\0Brine\0"
        "Cress\0Dapple\0Esker\0Fallow\0Gannet\0Hearth\0Ilex\0Jasper\0Kettle\0"
        "Larch\0Mallow\0Nettle\0Ochre\0Plover\0Quill\0Runnel\0Sedge\0Tamar\0"
        "Umber\0Vesper\0Wren\0Yarrow";
    static const char ends[]="Reach\0Orbital\0Quay\0Relay\0Anchor\0Landing\0Harbour\0Watch";
    uint32_t r=cg_hash(seed^0x53544154u);
    snprintf(out,20,"%s %s",part(roots,r%40),part(ends,(r/40)%8));
}

/* Twelve reachable openings per role, all <=30 characters. */
static const char trader[]=
    "Got room for a little luck?\0My manifest has a loose end.\0A fair signal to you.\0"
    "The holds smell of copper.\0I saved the good tea.\0Your wake looks expensive.\0"
    "Another port, another ledger.\0Can you hear my kettle?\0We trade by spoken promise.\0"
    "Mind the crates on approach.\0Fresh seals on every crate.\0What does your hold need?";
static const char pirate[]=
    "Quiet engines. Open holds.\0Your freight pays our rent.\0This lane has a toll.\0"
    "Keep your hands in view.\0We counted your gun ports.\0Nice ship. Costly passage.\0"
    "The pack wants its supper.\0Don't make us spill the tea.\0No hero songs today.\0"
    "Our ledger has your wake.\0You have something we need.\0Let's settle this breathing.";
static const char police[]=
    "Patrol requests your seal.\0Hold for a registry check.\0Keep the traffic lane open.\0"
    "Duty desk, receiving you.\0We have your arrival logged.\0Please lower your weapons.\0"
    "Show us a tidy manifest.\0A short check, then onward.\0Patrol has a quiet channel.\0"
    "Your transponder reads well.\0Stay steady for our scanner.\0Control has your flight path.";
static const char rescue[]=
    "Our pumps have stopped.\0Is somebody receiving us?\0The cabin is getting cold.\0"
    "We can see your running lamp.\0Please don't leave our wake.\0Our supplies are almost gone.\0"
    "We have children aboard.\0A little food buys us time.\0Life support is on reserve.\0"
    "We're sharing the last cup.\0The crew is still together.\0We held a light for you.";
static const char derelict[]=
    "Recorder: last watch begins.\0Cargo seals remain intact.\0A kettle clicks in the dark.\0"
    "No crew voices on this band.\0Log fragment: a broken tow.\0The bridge answers in static.\0"
    "An empty suit guards the helm.\0Last meal left on the table.\0An old freight seal repeats.\0"
    "Emergency cells still hum.\0A scratched chart survives.\0The hold latch is unsecured.";
static const char beacon[]=
    "Local relay, good reception.\0Port bulletin follows.\0Welcome to the public band.\0"
    "Relay keeper here. Tea?\0Navigation signal is steady.\0Today's notices are ready.\0"
    "We keep a light for strays.\0Fresh reports on this channel.\0Your signal reached the quay.\0"
    "Public ledger is now open.\0Another voice in the quiet.\0We heard you before the port.";
const char *cg_lore_open(game_role role,uint32_t seed) {
    const char *bank=role==ROLE_TRADER?trader:role==ROLE_PIRATE?pirate:role==ROLE_POLICE?police:
        role==ROLE_RESCUE?rescue:role==ROLE_DERELICT?derelict:beacon;
    return part(bank,seed%12);
}
const char *cg_lore_outcome(int outcome,uint32_t seed) {
    static const char paid[]="Seals exchanged. Safe passage.\0Our ledger balances.\0Fair dealing travels far.\0The crew sends its thanks.";
    static const char refused[]="We will remember this wake.\0Keep that channel open.\0Your choice has been logged.\0Then mind the next crossing.";
    static const char helped[]="We remember your help.\0You kept our lamps burning.\0There is always tea for you.\0Our crew owes you a quiet day.";
    static const char wanted[]="Your registry raises a flag.\0Patrol has marked your wake.\0Your papers need attention.\0Your flight path was reported.";
    static const char lawless[]="No courts watch this lane.\0Keep your own witness here.\0A promise needs a strong hull.\0The quay settles its debts.";
    static const char lawful[]="Port control is listening.\0The lane watch keeps records.\0File your seals on arrival.\0The public band stays open.";
    static const char house[]="Our house honours its word.\0The guild has your signal.\0A shared seal opens doors.\0Our convoy keeps its accounts.";
    const char *b=outcome==0?paid:outcome==1?refused:outcome==2?helped:outcome==3?wanted:
        outcome==4?lawless:outcome==5?lawful:house;
    return part(b,seed%4);
}
void cg_news(void) {
    game_system *home=&cg->v.systems[cg->v.system];uint32_t day=cg->v.market_day;
    uint32_t r=cg_hash(home->seed^cg_hash(day));int dst=-1;
    float nearest=1e9f;
    for(int i=0;i<GAME_SYSTEMS;i++) if(i!=cg->v.system) {
        float d=game_distance(home,&cg->v.systems[i]);
        if(d<nearest) {nearest=d;dst=i;}
    }
    snprintf(cg->v.news[0],41,"%s",cg->v.event_text);
    int good=(int)(r%GAME_GOODS);
    if(dst>=0) snprintf(cg->v.news[1],41,"%.15s %.12s %dcr",cg->v.systems[dst].name,
        cg->v.market[good].name,game_price(&cg->v.systems[dst],good,day));
    /* Public notices depend only on system/day, never acceptance order. */
    game_mission offer;cg_mission_offer(4,&offer);
    snprintf(cg->v.news[2],41,"Job: %.24s %ucr",offer.title,(unsigned)offer.reward);
    snprintf(cg->v.news[3],41,"%s",part(quirk,cg_hash(r)%40));
}

/* Eight economy notes for each good; packed strings, no per-frame formatting. */
const char *game_good_note(int good) {
    static const char food[]="Shipyard crews buy bulk\0Canteens need fresh grain\0Mill shifts eat late\0City ovens run all night\0Orchard boats bring surplus\0Local grain fills the silos\0Every farm has a surplus\0Harvest sacks crowd the quay";
    static const char textiles[]="Suits need heatproof lining\0Uniform looms run daily\0Factory cloth is plentiful\0City tailors buy strong yarn\0Fruit nets use local fibre\0Looms share the harvest shed\0Spun stalk fibre costs little\0Home looms supply the port";
    static const char isotope[]="Reactor cells made nearby\0Shielded mills refine ore\0Hospitals share reactor lots\0City clinics need isotopes\0Crop labs bid for tracers\0Irrigation labs buy small lots\0Medical imports arrive rarely\0Farm clinics await shipments";
    static const char liquor[]="Crews toast each hull launch\0Distillers buy waste heat\0Shift bars buy full casks\0Rooftop taverns set the price\0Orchard cider flows freely\0Fruit stills fill spare casks\0Grain spirits fill the cellars\0Harvest mash is plentiful";
    static const char luxury[]="Hull owners furnish cabins\0Factory patrons buy gifts\0Designer workshops export\0City salons reserve the best\0Orchard estates import gifts\0Craft fairs draw rich buyers\0Traders bring rare comforts\0Few imports reach the farms";
    static const char narcotic[]="Hidden in reactor packing\0Smuggled past shift gates\0Black market pays in seals\0Patrol searches city freight\0Mixed with orchard remedies\0Crop inspectors watch closely\0Unlicensed seed labs abound\0Field stills evade the census";
    static const char computers[]="Local chip fabs run hot\0Shipyard cores are plentiful\0Assembly lines export boards\0City networks buy upgrades\0Farm controls need imports\0Orchard sorters need chips\0Harvest bots await new cores\0Few repair shops stock chips";
    static const char machine[]="Shipyard presses make spares\0Local foundries cut costs\0Mill gear is built nearby\0Workshops trade rebuilt tools\0Packing sheds need new belts\0Harvesters need imported parts\0Irrigation pumps wear quickly\0Tractor parts cross the void";
    static const char alloy[]="Hull offcuts flood the market\0Local smelters pour daily\0Rolling mills supply sheets\0Builders buy strong beams\0Shed roofs need imported plate\0Packing lines consume sheets\0Plough makers import steel\0Farm repairs use scrap first";
    static const char arms[]="Armourers share foundry runs\0Guns leave disguised as tools\0Private guards bid quietly\0City permits exclude this lot\0Estate guards buy off ledger\0Rural patrols watch gun crates\0Raid fears feed covert demand\0Unlicensed guns risk seizure";
    static const char fur[]="Synthetic pelts made nearby\0Cabin liners use warm fibre\0Factory dyes colour the hides\0Fashion houses bid at dusk\0Orchard pests yield soft hides\0Trappers trade at dawn\0Local pelts fill drying racks\0Winter hides are plentiful";
    static const char mineral[]="Foundries swallow ore daily\0Ore docks have steady buyers\0Mill furnaces need clean ore\0Building booms consume stone\0Soil blends need ground rock\0Local quarries supply gravel\0Fieldstone is easy to gather\0Farm carts bring quarry waste";
    static const char gold[]="Circuit fabs buy clean gold\0Contacts use thin gold foil\0Refiners recover mill scrap\0City jewellers buy bright bars\0Wedding gifts cross the docks\0Orchard vaults buy small bars\0Prospectors trade river flakes\0Creek gold pays harvest debts";
    static const char platinum[]="Fuel cells need pure metal\0Local catalysts use platinum\0Refiners trade catalyst scrap\0Clinics order fine electrodes\0Water filters need catalysts\0Imports supply orchard labs\0Rare shipments sell quickly\0Farm buyers pool their orders";
    static const char gem[]="Tool cutters buy hard stones\0Laser shops sort each crystal\0Industrial gems are graded\0City cutters add a premium\0Local caves supply rough gems\0Orchard families trade stones\0River sieves yield bright grit\0Loose gems pay field rents";
    static const char relic[]="Labs buy unlabelled relics\0Shipyards test old fragments\0Museum crates draw collectors\0City archives bid on oddities\0Old ruins border the groves\0Dig finds ride fruit barges\0Ploughs uncover strange pieces\0Farmers trade buried curios";
    if(!cg || good<0 || good>=GAME_GOODS) return "";
    const char *b=good==0?food:good==1?textiles:good==2?isotope:good==3?liquor:
        good==4?luxury:good==5?narcotic:good==6?computers:good==7?machine:good==8?alloy:
        good==9?arms:good==10?fur:good==11?mineral:good==12?gold:good==13?platinum:good==14?gem:relic;
    return part(b,cg->v.systems[cg->v.system].economy%8);
}
