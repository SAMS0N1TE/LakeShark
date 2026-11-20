/* MESHTASTIC's other translator languages (app-sdk/apps/meshtastic/mt_xlate_lang.c
   and the card files lang/<code>.bin): each language's detection on short chat
   lines and a handful of gists, detection across all of them without calling
   Spanish or English lines anything else, the card file checks, the
   phrasebooks, and the limits. Phrase-table lines must come out exactly;
   rule-built lines must carry the key words. */

#include "ls_test.h"
#include "mt_xlate.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MT_LANG_DIR
#define MT_LANG_DIR "../app-sdk/apps/meshtastic/lang"
#endif

static char out[1024];
static void *file[MT_LANG_COUNT];
static size_t file_n[MT_LANG_COUNT];

static void *load(const char *code, size_t *n)
{
    char path[512];
    FILE *f;
    void *b;
    snprintf(path, sizeof path, "%s/%s.bin", MT_LANG_DIR, code);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n ? *n : 1);
    if (!b || fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

/* every card language attached, once */
static int attach_all(void)
{
    static int done, ok;
    int l;
    if (done) return ok;
    done = ok = 1;
    for (l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++) {
        file[l] = load(mt_lang_code((mt_lang_t)l), &file_n[l]);
        if (!file[l] || mt_xlate_lang_attach((mt_lang_t)l, file[l], file_n[l]) != 0) ok = 0;
    }
    return ok;
}

static const char *tr(mt_lang_t l, const char *s)
{
    mt_xlate_to_en(l, s, out, sizeof out);
    return out;
}

static int has_all(const char *s, const char *words)
{
    char w[64];
    while (*words) {
        int n = 0;
        while (words[n] && words[n] != '|') n++;
        snprintf(w, sizeof w, "%.*s", n, words);
        {
            const char *p = s;
            int found = 0;
            size_t m = strlen(w);
            for (; *p; p++)
                if (!strncasecmp(p, w, m)) { found = 1; break; }
            if (!found) return 0;
        }
        words += n;
        if (*words == '|') words++;
    }
    return 1;
}

#define EXACT(l, s, en) LS_EQ_STR(tr(l, s), en)
#define HAS(l, s, words) LS_CHECK_MSG(has_all(tr(l, s), words), "[%s] -> [%s] lacks %s", s, out, words)
#define LACKS(l, s, word) LS_CHECK_MSG(!has_all(tr(l, s), word), "[%s] -> [%s] has %s", s, out, word)

static void detects(mt_lang_t want, const char *const *lines, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        int c = -1;
        const mt_lang_t l = mt_xlate_detect(lines[k], &c);
        LS_CHECK_MSG(l == want, "[%s] read as %s (%d%%), not %s", lines[k], mt_lang_code(l), c, mt_lang_code(want));
        LS_CHECK_MSG(l != want || (c >= MT_XLATE_MIN_CONF && c <= 100), "[%s] confidence %d", lines[k], c);
    }
}
#define DETECTS(l, ...) do { static const char *const L_[] = { __VA_ARGS__ }; detects(l, L_, (int)(sizeof L_ / sizeof L_[0])); } while (0)

/* --------------------------------------------------------------- files */

LS_CASE(card_files_attach_and_bad_ones_are_refused)
{
    static uint32_t buf[64];
    size_t n = 0;
    void *b;
    LS_CHECK_MSG(attach_all(), "lang/*.bin missing or refused in %s", MT_LANG_DIR);
    LS_CHECK(mt_xlate_lang_ready(MT_LANG_ES));          /* built in */
    LS_CHECK(mt_xlate_lang_ready(MT_LANG_PT) && mt_xlate_lang_ready(MT_LANG_NL));
    LS_CHECK(!mt_xlate_lang_ready(MT_LANG_EN) && !mt_xlate_lang_ready(MT_LANG_UNKNOWN));
    /* the wrong language's file, a cut one, junk, NULL, a language without a file */
    LS_CHECK(mt_xlate_lang_attach(MT_LANG_FR, file[MT_LANG_PT], file_n[MT_LANG_PT]) < 0);
    LS_CHECK(mt_xlate_lang_attach(MT_LANG_PT, file[MT_LANG_PT], 200) < 0);
    LS_CHECK(mt_xlate_lang_attach(MT_LANG_PT, file[MT_LANG_PT], file_n[MT_LANG_PT] / 2) < 0);
    memset(buf, 0xA5, sizeof buf);
    LS_CHECK(mt_xlate_lang_attach(MT_LANG_PT, buf, sizeof buf) < 0);
    LS_CHECK(mt_xlate_lang_attach(MT_LANG_PT, NULL, 1000) < 0);
    LS_CHECK(mt_xlate_lang_attach(MT_LANG_ES, file[MT_LANG_PT], file_n[MT_LANG_PT]) < 0);
    /* a refused attach keeps what was there */
    LS_CHECK(mt_xlate_lang_ready(MT_LANG_PT) && mt_xlate_lang_ready(MT_LANG_FR));
    /* detached: nothing comes out, and it comes back */
    mt_xlate_lang_detach(MT_LANG_IT);
    LS_CHECK(!mt_xlate_lang_ready(MT_LANG_IT));
    LS_EQ_INT(mt_xlate_to_en(MT_LANG_IT, "Ciao, come stai?", out, sizeof out), 0);
    LS_EQ_STR(out, "");
    LS_EQ_INT(mt_xlate_lang_attach(MT_LANG_IT, file[MT_LANG_IT], file_n[MT_LANG_IT]), 0);
    LS_CHECK(mt_xlate_to_en(MT_LANG_IT, "Ciao, come stai?", out, sizeof out) > 0);
    /* every byte flipped in turn in the header and the first tables: refused or harmless */
    b = load("de", &n);
    LS_CHECK(b != NULL);
    if (b) {
        size_t i;
        for (i = 0; i < 512 && i < n; i++) {
            ((unsigned char *)b)[i] ^= 0xFF;
            if (mt_xlate_lang_attach(MT_LANG_DE, b, n) == 0)
                mt_xlate_to_en(MT_LANG_DE, "Ich bin auf dem Berg mit dem Funkgerät, hörst du mich?", out, sizeof out);
            ((unsigned char *)b)[i] ^= 0xFF;
        }
        LS_EQ_INT(mt_xlate_lang_attach(MT_LANG_DE, file[MT_LANG_DE], file_n[MT_LANG_DE]), 0);
        free(b);
    }
}

/* ---------------------------------------------------------- Portuguese */

LS_CASE(portuguese)
{
    attach_all();
    DETECTS(MT_LANG_PT, "Oi, tudo bem?", "vc ta onde?", "blz, valeu", "Não tenho sinal aqui", "Eles chegaram ontem",
            "Minha bateria está fraca", "kkkk muito bom", "Obrigado pela ajuda");
    EXACT(MT_LANG_PT, "Oi, tudo bem?", "Hi, how are you?");
    EXACT(MT_LANG_PT, "Você me ouve?", "Do you hear me?");
    EXACT(MT_LANG_PT, "Te ouço alto e claro", "I hear you loud and clear");
    HAS(MT_LANG_PT, "Estou na praça perto da estação", "I am in the square|near the station");
    HAS(MT_LANG_PT, "Minha bateria está fraca", "my battery is weak");
    HAS(MT_LANG_PT, "Não consigo ouvir você", "I can't hear you");
    HAS(MT_LANG_PT, "O repetidor do morro voltou a funcionar", "hill repeater");
    HAS(MT_LANG_PT, "Me manda sua localização", "send me your location");
    LACKS(MT_LANG_PT, "Me manda sua localização", "he sends");
    HAS(MT_LANG_PT, "Amanhã vai chover", "tomorrow it is going to rain");
    HAS(MT_LANG_PT, "Tenho que ir", "I have to go");
    HAS(MT_LANG_PT, "vc ta onde?", "where are you");
    HAS(MT_LANG_PT, "O nó da praça não funciona", "node|doesn't work");
}

/* -------------------------------------------------------------- French */

LS_CASE(french)
{
    attach_all();
    DETECTS(MT_LANG_FR, "Salut, ça va ?", "slt cv ?", "Je te reçois fort et clair", "Il n'y a pas de réseau ici",
            "J'ai besoin d'aide", "mdr trop bien", "Où es-tu ?", "Ils sont arrivés hier");
    EXACT(MT_LANG_FR, "Salut, ça va ?", "Hi, how are you?");
    EXACT(MT_LANG_FR, "Tu m'entends ?", "Do you hear me?");
    EXACT(MT_LANG_FR, "Où es-tu ?", "Where are you?");
    HAS(MT_LANG_FR, "Il n'y a pas de réseau ici", "there is no network here");
    HAS(MT_LANG_FR, "Je ne t'entends pas", "I don't hear you");
    HAS(MT_LANG_FR, "J'ai acheté une nouvelle antenne", "a new antenna");
    LACKS(MT_LANG_FR, "J'ai acheté une nouvelle antenne", "news");
    HAS(MT_LANG_FR, "Le relais de la colline ne marche plus", "doesn't work anymore");
    HAS(MT_LANG_FR, "Ils sont arrivés hier", "they arrived yesterday");
    HAS(MT_LANG_FR, "Envoie-moi ta position", "send me your location");
    HAS(MT_LANG_FR, "Je suis en train de charger", "I am charging");
    HAS(MT_LANG_FR, "Est-ce que tu as du signal ?", "do you have any signal");
    HAS(MT_LANG_FR, "Demain il va pleuvoir", "it is going to rain");
}

/* -------------------------------------------------------------- German */

LS_CASE(german)
{
    attach_all();
    DETECTS(MT_LANG_DE, "Hallo, wie geht's?", "Hörst du mich?", "moin, alles klar?", "Mein Akku ist fast leer",
            "Hier gibt es kein Netz", "Ich kann dich nicht hören", "Wann kommst du?", "Bin gleich da");
    EXACT(MT_LANG_DE, "Hörst du mich?", "Do you hear me?");
    EXACT(MT_LANG_DE, "Ich höre dich laut und deutlich", "I hear you loud and clear");
    HAS(MT_LANG_DE, "Ich kann dich nicht hören", "I can't hear you");
    HAS(MT_LANG_DE, "Ich habe eine neue Antenne gekauft", "I bought a new antenna");     /* the participle comes forward */
    HAS(MT_LANG_DE, "Ich rufe dich morgen an", "call you tomorrow");                     /* rufe ... an: anrufen */
    HAS(MT_LANG_DE, "Weil ich keinen Empfang habe, schreibe ich später", "because I have no reception|I write later");
    HAS(MT_LANG_DE, "Morgen fahre ich zum Berg", "tomorrow I drive");                   /* the verb second, the subject first */
    HAS(MT_LANG_DE, "Mein Funkgerät ist kaputt", "my radio is broken");
    HAS(MT_LANG_DE, "Die Akkuladung ist niedrig", "battery charge");                    /* a compound split */
    HAS(MT_LANG_DE, "Ich habe das nicht gesehen", "didn't see");
    HAS(MT_LANG_DE, "fur dich", "for you");                                              /* no umlaut typed */
}

/* ------------------------------------------------------------- Italian */

LS_CASE(italian)
{
    attach_all();
    DETECTS(MT_LANG_IT, "Ciao, come stai?", "Ti sento forte e chiaro", "La mia batteria è scarica", "C'è qualcuno?",
            "cmq tutto ok", "xké non rispondi?", "Domani pioverà", "Sono arrivati ieri");
    EXACT(MT_LANG_IT, "Ciao, come stai?", "Hi, how are you?");
    EXACT(MT_LANG_IT, "C'è qualcuno?", "Is anyone there?");
    EXACT(MT_LANG_IT, "Dove sei?", "Where are you?");
    HAS(MT_LANG_IT, "Mandami la tua posizione", "send me your location");              /* mandami: manda + mi */
    HAS(MT_LANG_IT, "Non riesco a sentirti", "I can't hear you");
    HAS(MT_LANG_IT, "Sono arrivati ieri", "they arrived yesterday");
    HAS(MT_LANG_IT, "Il ripetitore della collina non funziona più", "hill repeater doesn't work anymore");
    HAS(MT_LANG_IT, "Ho comprato una nuova antenna", "I bought a new antenna");
    HAS(MT_LANG_IT, "Sono appena arrivato", "I just arrived");
    HAS(MT_LANG_IT, "Lui non lo sa", "he doesn't know it");
    HAS(MT_LANG_IT, "Aspettami davanti alla stazione", "wait for me|station");
}

/* --------------------------------------------------------------- Dutch */

LS_CASE(dutch)
{
    attach_all();
    DETECTS(MT_LANG_NL, "Hoi, hoe gaat het?", "Hoor je me?", "Mijn batterij is bijna leeg", "Is er iemand?",
            "Ik heb hulp nodig", "Ze zijn gisteren aangekomen", "ff wachten", "Waar ben je?");
    EXACT(MT_LANG_NL, "Hoi, hoe gaat het?", "Hi, how are you?");
    EXACT(MT_LANG_NL, "Hoor je me?", "Do you hear me?");
    EXACT(MT_LANG_NL, "Waar ben je?", "Where are you?");
    HAS(MT_LANG_NL, "Ik kan je niet horen", "I can't hear you");
    HAS(MT_LANG_NL, "Ik heb een nieuwe antenne gekocht", "I bought a new antenna");
    HAS(MT_LANG_NL, "Ik heb hulp nodig", "I need help");                                /* nodig hebben */
    HAS(MT_LANG_NL, "Ik bel je morgen op", "call you tomorrow");                         /* bel ... op: opbellen */
    HAS(MT_LANG_NL, "Ze zijn gisteren aangekomen", "they arrived yesterday");
    HAS(MT_LANG_NL, "Morgen gaat het regenen", "it is going to rain");
    HAS(MT_LANG_NL, "Stuur me je locatie", "send me your location");
}

/* -------------------------------------------------------------- Polish */

LS_CASE(polish)
{
    char s[96];
    attach_all();
    DETECTS(MT_LANG_PL, "Cześć, jak się masz?", "Gdzie jesteś?", "Słyszysz mnie?", "Nie mam zasięgu", "Potrzebuję pomocy",
            "Jestem zmęczony", "Gdzie jestes?", "Dzieki, na razie", "Wszystko ok, jade do domu");
    EXACT(MT_LANG_PL, "Cześć, jak się masz?", "Hi, how are you?");
    EXACT(MT_LANG_PL, "Słyszysz mnie?", "Do you hear me?");
    EXACT(MT_LANG_PL, "Gdzie jesteś?", "Where are you?");
    HAS(MT_LANG_PL, "Potrzebuję pomocy", "I need help");
    HAS(MT_LANG_PL, "Słabo cię słyszę", "I hear you");                    /* -ę: the first person */
    HAS(MT_LANG_PL, "Nie slysze cie", "I don't hear you");                 /* typed without the Polish letters */
    HAS(MT_LANG_PL, "Jadę do pracy", "I drive|to work");
    HAS(MT_LANG_PL, "Czekam na ciebie", "I wait for you");
    HAS(MT_LANG_PL, "Jestem zmęczony", "I am tired");
    HAS(MT_LANG_PL, "Sygnał jest dobry", "signal is good");
    HAS(MT_LANG_PL, "Jutro rano idę w góry", "tomorrow|in the morning|I go");
    /* the screen: l with stroke as l, the other marks off */
    strcpy(s, "Zażółć gęślą jaźń, Łódź");
    mt_xlate_fold_ascii(s);
    LS_EQ_STR(s, "Zazolc gesla jazn, Lodz");
    LS_CHECK(mt_xlate_needs_screen("Łódź") && !mt_xlate_needs_screen("Lodz"));
    mt_xlate_screen("Słyszę cię, Łukasz", MT_LANG_PL, s, sizeof s);
    LS_EQ_STR(s, "Slysze cie, Lukasz");
}

/* ------------------------------------------------- Russian and Ukrainian */

LS_CASE(russian)
{
    attach_all();
    DETECTS(MT_LANG_RU, "Привет, как дела?", "Ты меня слышишь?", "Где ты?", "Спасибо большое", "Нет связи",
            "Всё в порядке?", "Мне нужна помощь", "Жду тебя", "privet, kak dela?", "spasibo, vse normalno");
    EXACT(MT_LANG_RU, "Привет, как дела?", "Hi, how are you?");
    EXACT(MT_LANG_RU, "Ты меня слышишь?", "Do you hear me?");
    EXACT(MT_LANG_RU, "Где ты?", "Where are you?");                        /* no "are" in Russian */
    HAS(MT_LANG_RU, "Я дома", "I am at home");
    HAS(MT_LANG_RU, "Сигнал хороший", "signal is good");
    HAS(MT_LANG_RU, "Мне нужна помощь", "I need help");
    HAS(MT_LANG_RU, "Жду тебя", "I wait for you");
    HAS(MT_LANG_RU, "Батарея почти села", "battery|almost");
    HAS(MT_LANG_RU, "Нет связи", "no connection");
    HAS(MT_LANG_RU, "Встретимся в восемь", "we will meet at eight");
    HAS(MT_LANG_RU, "privet, kak dela?", "hi, how are you");               /* chat Latin */
    HAS(MT_LANG_RU, "ya doma, vse horosho", "I am at home|good");          /* h for kh */
    LACKS(MT_LANG_RU, "ya doma, vse horosho", "horosho");
}

LS_CASE(ukrainian)
{
    attach_all();
    DETECTS(MT_LANG_UK, "Привіт, як справи?", "Ти мене чуєш?", "Дякую дуже", "Немає зв'язку", "Мені потрібна допомога",
            "Де ти?", "Їду на роботу", "pryvit, yak spravy?", "dyakuyu, vse dobre");
    EXACT(MT_LANG_UK, "Привіт, як справи?", "Hi, how are you?");
    EXACT(MT_LANG_UK, "Ти мене чуєш?", "Do you hear me?");
    EXACT(MT_LANG_UK, "Де ти?", "Where are you?");
    HAS(MT_LANG_UK, "Я вдома", "I am at home");
    HAS(MT_LANG_UK, "Мені потрібна допомога", "I need help");
    HAS(MT_LANG_UK, "Чекаю на тебе", "I wait for you");
    HAS(MT_LANG_UK, "Батарея майже розряджена", "battery is almost flat");
    HAS(MT_LANG_UK, "Немає зв'язку", "no connection");
    HAS(MT_LANG_UK, "Завтра вранці йду в гори", "tomorrow|in the morning|I go");
}

LS_CASE(russian_or_ukrainian)
{
    static const char *const ru[] = {
        "Ты где?", "Это мы", "Идёт дождь", "Подъезжаю",                             /* ы э ё ъ */
        "Где встречаемся?", "Жду тебя", "Спокойной ночи", "Доброе утро", "Позвони мне", "Увидимся завтра",
        "Сейчас буду", "Что делаешь?", "Я устал",
    };
    static const char *const uk[] = {
        "Привіт", "Їду додому", "Ти мене чуєш?", "Ґанок",                           /* і ї є ґ */
        "Де ти?", "Дякую дуже", "Що робиш?", "Буду за годину", "Все гаразд?", "Добрий ранок", "Побачимося завтра",
        "Я втомився", "Зараз буду",
    };
    unsigned k;
    char b[256];
    mt_lang_t l;
    int c;
    attach_all();
    for (k = 0; k < sizeof ru / sizeof ru[0]; k++) {
        l = mt_xlate_detect(ru[k], &c);
        LS_CHECK_MSG(l == MT_LANG_RU, "[%s] read as %s (%d%%)", ru[k], mt_lang_code(l), c);
    }
    for (k = 0; k < sizeof uk / sizeof uk[0]; k++) {
        l = mt_xlate_detect(uk[k], &c);
        LS_CHECK_MSG(l == MT_LANG_UK, "[%s] read as %s (%d%%)", uk[k], mt_lang_code(l), c);
    }
    /* a line both could have written is still read, by the one that is on */
    l = mt_xlate_detect("Скоро буду", &c);
    LS_CHECK((l == MT_LANG_RU || l == MT_LANG_UK) && c >= MT_XLATE_MIN_CONF);
    LS_CHECK(mt_xlate_incoming_in("Скоро буду", MT_LANG_BIT(MT_LANG_UK), b, sizeof b, &l) > 0);
    LS_EQ_INT(l, MT_LANG_UK);
    LS_CHECK(mt_xlate_incoming_in("Скоро буду", MT_LANG_BIT(MT_LANG_RU), b, sizeof b, &l) > 0);
    LS_EQ_INT(l, MT_LANG_RU);
    /* but a letter only one of them has decides */
    LS_EQ_INT(mt_xlate_incoming_in("Їду додому", MT_LANG_BIT(MT_LANG_RU), b, sizeof b, &l), 0);
    LS_EQ_INT(l, MT_LANG_UK);
    /* Latin typed by a Polish, Spanish or English speaker is not Russian */
    LS_CHECK(mt_xlate_detect("Nie wiem, zapytam", 0) == MT_LANG_PL);
    LS_CHECK(mt_xlate_detect("da, vamos", 0) != MT_LANG_RU);
    LS_CHECK(mt_xlate_detect("net is down again", 0) != MT_LANG_RU);
}

/* Cyrillic on a Latin screen: transliterated, never question marks */
LS_CASE(cyrillic_on_the_screen)
{
    char s[256];
    int n = mt_phrase_count(), i, l;
    unsigned k;
    mt_xlate_screen("Привет, как дела?", MT_LANG_RU, s, sizeof s);
    LS_EQ_STR(s, "Privet, kak dela?");
    mt_xlate_screen("Привіт, як справи?", MT_LANG_UK, s, sizeof s);
    LS_EQ_STR(s, "Pryvit, yak spravy?");
    mt_xlate_screen("Где ты? Щас буду", MT_LANG_UNKNOWN, s, sizeof s);
    LS_EQ_STR(s, "Gde ty? Shchas budu");
    mt_xlate_screen("Буду за годину", MT_LANG_UK, s, sizeof s);           /* г is h in Ukrainian */
    LS_EQ_STR(s, "Budu za hodynu");
    mt_xlate_screen("Немає зв'язку, ВСЁ ПЛОХО", MT_LANG_UNKNOWN, s, sizeof s);
    LS_EQ_STR(s, "Nemaye zv'yazku, VSYO PLOKHO");
    LS_CHECK(mt_xlate_needs_screen("Где ты?") && !mt_xlate_needs_screen("¿Dónde estás?"));
    mt_xlate_screen("Привет", MT_LANG_RU, s, 4);                          /* cut short, still a string */
    LS_CHECK(strlen(s) < 4);
    /* the in-place fold: at most two letters a Cyrillic letter, so it fits where the UTF-8 was */
    strcpy(s, "Щас, Жду");
    mt_xlate_fold_ascii(s);
    LS_EQ_STR(s, "Shas, Zhdu");
    /* every Russian and Ukrainian phrase: Latin only, and no question mark the line did not have */
    for (l = MT_LANG_RU; l <= MT_LANG_UK; l++)
        for (i = 0; i < n; i++) {
            const char *p = mt_phrase_in(i, (mt_lang_t)l);
            int q0 = 0, q1 = 0;
            mt_xlate_screen(p, (mt_lang_t)l, s, sizeof s);
            for (k = 0; p[k]; k++) q0 += p[k] == '?';
            for (k = 0; s[k]; k++) {
                q1 += s[k] == '?';
                LS_CHECK_MSG((unsigned char)s[k] < 0x80, "%s phrase %d: [%s]", mt_lang_code((mt_lang_t)l), i, s);
            }
            LS_CHECK_MSG(q0 == q1, "%s phrase %d: [%s]", mt_lang_code((mt_lang_t)l), i, s);
        }
}

/* -------------------------------------------- Spanish and English stay */

LS_CASE(spanish_and_english_lines_stay_spanish_and_english)
{
    static const char *const es[] = {
        "hola", "gracias", "hola che", "todo bien?", "estoy en la plaza", "no tengo batería", "dnd estas",
        "q onda", "te copio", "me escuchas?", "mañana subo al cerro", "dale nos vemos", "se cortó la luz",
        "Necesito ayuda", "voy para alla", "buen dia", "¿Dónde estás?", "jajaja buenisimo che",
        "Me queda poca batería", "Re piola tu nodo", "El bondi no anda", "Hay un herido en el cerro",
        "Avisame cuando llegues porfa", "Compré una antena nueva", "Tengo que irme",
    };
    static const char *const en[] = {
        "hello", "see you", "Where are you?", "No signal here", "Copy that", "Can you hear me?", "My battery is low",
        "I need help", "Is anyone on the mesh tonight?", "che, the antenna is broken again", "mesh is quiet today",
        "Hello from Berlin", "roger that, heading back to base", "looking good on my side", "Signal is weak here",
    };
    unsigned k;
    attach_all();
    for (k = 0; k < sizeof es / sizeof es[0]; k++) {
        int c = -1;
        const mt_lang_t l = mt_xlate_detect(es[k], &c);
        LS_CHECK_MSG(l == MT_LANG_ES, "[%s] read as %s (%d%%)", es[k], mt_lang_code(l), c);
    }
    for (k = 0; k < sizeof en / sizeof en[0]; k++) {
        int c = -1;
        const mt_lang_t l = mt_xlate_detect(en[k], &c);
        LS_CHECK_MSG(l == MT_LANG_EN || l == MT_LANG_UNKNOWN, "[%s] read as %s (%d%%)", en[k], mt_lang_code(l), c);
    }
    LS_EQ_INT(mt_xlate_detect("ok", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(mt_xlate_detect("123", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(mt_xlate_detect("👍👍", 0), MT_LANG_UNKNOWN);
}

LS_CASE(incoming_over_a_set_of_languages)
{
    char b[256];
    mt_lang_t l = MT_LANG_EN;
    const unsigned es_pt = MT_LANG_BIT(MT_LANG_ES) | MT_LANG_BIT(MT_LANG_PT);
    attach_all();
    LS_CHECK(mt_xlate_incoming_in("Oi, tudo bem?", es_pt, b, sizeof b, &l) > 0);
    LS_EQ_INT(l, MT_LANG_PT);
    LS_EQ_STR(b, "Hi, how are you?");
    /* German heard while only Spanish and Portuguese are read: named, not translated, not called Spanish */
    LS_EQ_INT(mt_xlate_incoming_in("Hörst du mich?", es_pt, b, sizeof b, &l), 0);
    LS_EQ_INT(l, MT_LANG_DE);
    LS_EQ_STR(b, "");
    LS_CHECK(mt_xlate_incoming_in("estoy en la plaza", es_pt, b, sizeof b, &l) > 0);
    LS_EQ_INT(l, MT_LANG_ES);
    LS_EQ_INT(mt_xlate_incoming_in("I'm at the plaza", es_pt, b, sizeof b, &l), 0);
    /* the 1.3 call still means Spanish only */
    LS_EQ_INT(mt_xlate_incoming("Oi, tudo bem?", b, sizeof b), 0);
    LS_CHECK(mt_xlate_incoming("estoy en la plaza", b, sizeof b) > 0);
}

/* ----------------------------------------------------------- phrasebooks */

LS_CASE(phrasebooks_in_every_language)
{
    int n = mt_phrase_count(), i, l;
    attach_all();
    LS_CHECK(n >= 40);
    for (l = MT_LANG_EN; l < MT_LANG_COUNT; l++) {
        int back = 0;
        for (i = 0; i < n; i++) {
            const char *s = mt_phrase_in(i, (mt_lang_t)l);
            LS_CHECK_MSG(s && s[0], "phrase %d has no %s", i, mt_lang_code((mt_lang_t)l));
            /* sent as written: Russian and Ukrainian as Cyrillic UTF-8 (two bytes a letter), well inside a packet */
            LS_CHECK_MSG(strlen(s) <= 160, "%s phrase %d is %u bytes", mt_lang_code((mt_lang_t)l), i, (unsigned)strlen(s));
            if (l == MT_LANG_RU || l == MT_LANG_UK)
                LS_CHECK_MSG((unsigned char)s[0] == 0xD0 || (unsigned char)s[0] == 0xD1, "%s phrase %d [%s]",
                             mt_lang_code((mt_lang_t)l), i, s);
            if (l >= MT_LANG_FIRST_CARD) {
                /* what is sent reads back as English with a word of the line */
                const char *e = mt_phrase(i).en;
                char w[32];
                int found = 0;
                mt_xlate_to_en((mt_lang_t)l, s, out, sizeof out);
                while (*e && !found) {
                    int m = 0;
                    while (e[m] && ((e[m] >= 'a' && e[m] <= 'z') || (e[m] >= 'A' && e[m] <= 'Z'))) m++;
                    if (m >= 4) { snprintf(w, sizeof w, "%.*s", m, e); found = has_all(out, w); }
                    e += m ? m : 1;
                }
                back += found || strlen(mt_phrase(i).en) < 6;
            }
        }
        if (l >= MT_LANG_FIRST_CARD)
            LS_CHECK_MSG(back * 10 >= n * 6, "%s: only %d of %d phrasebook lines read back", mt_lang_code((mt_lang_t)l), back, n);
    }
    LS_EQ_STR(mt_phrase_in(0, MT_LANG_EN), mt_phrase(0).en);
    LS_EQ_STR(mt_phrase_in(0, MT_LANG_ES), mt_phrase(0).es);
    LS_EQ_STR(mt_phrase_in(-1, MT_LANG_PT), "");
    LS_EQ_STR(mt_phrase_in(n, MT_LANG_DE), "");
    LS_EQ_STR(mt_phrase_in(0, MT_LANG_UNKNOWN), "");
    LS_EQ_STR(mt_lang_name(MT_LANG_IT), "ITALIAN");
    LS_EQ_INT(mt_lang_from_code("nl"), MT_LANG_NL);
    LS_EQ_INT(mt_lang_from_code("xx"), MT_LANG_UNKNOWN);
}

/* ---------------------------------------------------------------- limits */

LS_CASE(limits_tiny_buffers_long_and_strange_input)
{
    static char big[4096], res[8192];
    unsigned cap, k;
    int l, n;
    attach_all();
    for (l = MT_LANG_FIRST_CARD; l < MT_LANG_COUNT; l++) {
        static const char *const line[MT_LANG_COUNT] = { "", "", "", "Oi galera, tudo bem? Amanhã subo o morro com o rádio 👋",
            "Salut à tous, ça va ? Demain je monte sur la colline avec la radio 👋",
            "Hallo zusammen, wie geht's? Morgen gehe ich mit dem Funkgerät auf den Berg 👋",
            "Ciao a tutti, come va? Domani salgo sulla collina con la radio 👋",
            "Hoi allemaal, hoe gaat het? Morgen ga ik met de radio de heuvel op 👋",
            "Cześć wszystkim, jak leci? Jutro idę na górę z radiem 👋",
            "Всем привет, как дела? Завтра иду на холм с рацией 👋",
            "Всім привіт, як справи? Завтра йду на пагорб з рацією 👋" };
        for (cap = 1; cap < 48; cap++) {
            char g[80];
            memset(g, 0x5A, sizeof g);
            n = mt_xlate_to_en((mt_lang_t)l, line[l], g, cap);
            LS_CHECK_MSG(n >= 0 && (unsigned)n < cap, "%s cap %u returned %d", mt_lang_code((mt_lang_t)l), cap, n);
            LS_CHECK_MSG(g[n] == 0 && strlen(g) == (size_t)n, "%s cap %u not terminated", mt_lang_code((mt_lang_t)l), cap);
            for (k = cap; k < sizeof g; k++) LS_CHECK_MSG(g[k] == 0x5A, "%s cap %u wrote byte %u", mt_lang_code((mt_lang_t)l), cap, k);
        }
        big[0] = 0;
        while (strlen(big) + strlen(line[l]) + 2 < sizeof big - 1) { strcat(big, line[l]); strcat(big, ". "); }
        n = mt_xlate_to_en((mt_lang_t)l, big, res, sizeof res);
        LS_CHECK_MSG(n > 1000 && (size_t)n == strlen(res), "%s long input gave %d", mt_lang_code((mt_lang_t)l), n);
        LS_CHECK(has_all(res, "radio"));
        {
            static const char junk[] = "\xff\xfe ola \x80\x81 \xc3 aqui \x01\x02\x7f \xe2\x82 ok \xf0\x9f l'' -- d'-' ''' ---";
            n = mt_xlate_to_en((mt_lang_t)l, junk, res, sizeof res);
            LS_CHECK(n >= 0 && (size_t)n == strlen(res));
        }
        LS_EQ_INT(mt_xlate_to_en((mt_lang_t)l, "", res, sizeof res), 0);
        LS_EQ_INT(mt_xlate_to_en((mt_lang_t)l, NULL, res, sizeof res), 0);
        LS_EQ_INT(mt_xlate_to_en((mt_lang_t)l, "x", NULL, 10), 0);
        {
            char w[300];
            memset(w, 'a', sizeof w - 1);
            w[sizeof w - 1] = 0;
            n = mt_xlate_to_en((mt_lang_t)l, w, res, sizeof res);
            LS_EQ_INT(n, (int)strlen(w));                   /* one very long word passes through */
        }
        LS_EQ_STR(tr((mt_lang_t)l, "?!..."), "?!...");
    }
    /* the screen's fold: sharp s, oe and ae as two letters */
    {
        char s[64];
        strcpy(s, "Straße, cœur, Æsir, Grüße");
        mt_xlate_fold_ascii(s);
        LS_EQ_STR(s, "Strasse, coeur, AEsir, Grusse");
    }
}

LS_CASE(cost_of_a_full_message)
{
    static const char msg[] = "Boa noite a todos, alguém me escuta? Estou no morro perto da estação com pouca bateria, "
                              "amanhã volto com o rádio novo e aviso como chega o sinal daqui. Abraço e obrigado!";
    clock_t t0;
    int k;
    double us_det, us_tr;
    attach_all();
    t0 = clock();
    for (k = 0; k < 300; k++) mt_xlate_detect(msg, 0);
    us_det = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 300;
    t0 = clock();
    for (k = 0; k < 300; k++) mt_xlate_to_en(MT_LANG_PT, msg, out, sizeof out);
    us_tr = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 300;
    ls_note("%d bytes, every language attached: detect %.0f us, translate %.0f us on this host", (int)strlen(msg), us_det, us_tr);
    LS_CHECK_MSG(us_det < 5000 && us_tr < 5000, "detect %.0f us, translate %.0f us", us_det, us_tr);
}

LS_CASE(side_by_side)
{
    static const struct { mt_lang_t l; const char *s; } t[] = {
        { MT_LANG_PT, "Bom dia galera, alguém na escuta?" }, { MT_LANG_PT, "Estou chegando, me espera na praça" },
        { MT_LANG_FR, "Bonjour à tous, quelqu'un m'entend ?" }, { MT_LANG_FR, "Je suis sur la colline avec la radio" },
        { MT_LANG_DE, "Moin zusammen, hört mich jemand?" }, { MT_LANG_DE, "Ich bin auf dem Berg mit dem Funkgerät" },
        { MT_LANG_IT, "Ciao a tutti, qualcuno mi sente?" }, { MT_LANG_IT, "Sono sulla collina con la radio" },
        { MT_LANG_NL, "Hoi allemaal, hoort iemand mij?" }, { MT_LANG_NL, "Ik ben op de heuvel met de radio" },
        { MT_LANG_PL, "Cześć wszystkim, czy ktoś mnie słyszy?" }, { MT_LANG_PL, "Jestem na wzgórzu z radiem" },
        { MT_LANG_RU, "Всем привет, кто-нибудь меня слышит?" }, { MT_LANG_RU, "Я на холме с рацией" },
        { MT_LANG_UK, "Всім привіт, хтось мене чує?" }, { MT_LANG_UK, "Я на пагорбі з рацією" },
    };
    unsigned k;
    attach_all();
    printf("\n%-44s  %s\n", "line", "English");
    for (k = 0; k < sizeof t / sizeof t[0]; k++) {
        int c;
        const mt_lang_t l = mt_xlate_detect(t[k].s, &c);
        mt_xlate_to_en(t[k].l, t[k].s, out, sizeof out);
        printf("%-44s  %s   [%s %d%%]\n", t[k].s, out, mt_lang_code(l), c);
        LS_CHECK(out[0]);
        LS_CHECK_MSG(l == t[k].l, "[%s] read as %s", t[k].s, mt_lang_code(l));
    }
}
