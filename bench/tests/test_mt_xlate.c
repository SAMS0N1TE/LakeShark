/* MESHTASTIC's offline translator (app-sdk/apps/meshtastic/mt_xlate.c):
   language detection on short chat lines, the Spanish -> English gist on
   Argentine and general Latin American mesh chat, the phrasebook, and the
   limits (empty, long, tiny buffers, UTF-8, emoji, bytes that are not text).
   Phrase-table lines must come out exactly; rule-built lines must carry the
   key words. It also prints a side-by-side table to judge by reading. */

#include "ls_test.h"
#include "mt_xlate.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static char out[1024];

static const char *tr(const char *es)
{
    mt_xlate_es_en(es, out, sizeof out);
    return out;
}

/* every word of `words` (separated by |) appears in s, ignoring case */
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

#define EXACT(es, en) LS_EQ_STR(tr(es), en)
#define HAS(es, words) LS_CHECK_MSG(has_all(tr(es), words), "[%s] -> [%s] lacks %s", es, out, words)
#define LACKS(es, word) LS_CHECK_MSG(!has_all(tr(es), word), "[%s] -> [%s] has %s", es, out, word)

/* ------------------------------------------------------------- phrases */

LS_CASE(phrases_come_out_exactly)
{
    EXACT("Hola, ¿cómo andás?", "Hi, how are you doing?");
    EXACT("¿Dónde estás?", "Where are you?");
    EXACT("donde estas?", "where are you?");
    EXACT("por favor", "please");
    EXACT("Buenos días", "Good morning");
    EXACT("de nada", "you're welcome");
    EXACT("qué tal?", "how's it going?");
    EXACT("nos vemos", "see you");
    EXACT("ya está", "that's it");
    EXACT("dale", "ok");
    EXACT("q onda boludo", "what's up dude");
    EXACT("¿Me escuchás?", "Can you hear me?");
    EXACT("te escucho fuerte y claro", "I hear you loud and clear");
    EXACT("Copiado, gracias", "Copy that, thanks");
    EXACT("Recibido, cambio", "Received, over");
    EXACT("cambio y fuera", "over and out");
    EXACT("se cortó la luz", "the power went out");
    EXACT("No hay señal acá", "There's no signal here");
    EXACT("no sé", "I don't know");
    EXACT("ni idea", "no idea");
    EXACT("¡Feliz cumpleaños!", "Happy birthday!");
    EXACT("un abrazo", "hugs");
    EXACT("tranqui, no pasa nada", "relax, no worries");
    EXACT("Yo también", "Me too");
    EXACT("vamos a ver", "let's see");
    EXACT("a veces", "sometimes");
    EXACT("cerca de la plaza", "near the square");
}

/* ---------------------------------------------------- chat spellings */

LS_CASE(chat_spellings_and_laughter)
{
    HAS("q hacés?", "what are you up to");
    HAS("xq no venís?", "why|aren't you coming");
    HAS("pq no contestás?", "why|answer");
    HAS("porq no anda", "because|doesn't work");
    HAS("tb voy", "also|go");
    HAS("yo tmb", "me too");
    HAS("bn y vos?", "well|and you");
    HAS("dnd estás?", "where are you");
    HAS("xfa mandame un msj", "please|send me|message");
    HAS("porfa avisame", "please|let me know");
    HAS("nos vemos el finde jaja", "see you|weekend|haha");
    HAS("jejeje dale", "haha|ok");
    HAS("holaaaa", "hi");
    HAS("graciasss", "thanks");
    HAS("manana nos vemos", "tomorrow");     /* n-tilde typed as n */
    HAS("no tengo senal", "signal");
}

/* --------------------------------------------- Rioplatense and voseo */

LS_CASE(rioplatense_voseo_and_slang)
{
    HAS("che, vos tenés cargador?", "hey|do you have|charger");
    HAS("vos sabés dónde queda?", "you know|where");
    HAS("sos de acá?", "are you from here");
    HAS("andá a la plaza", "go to the square");
    HAS("decime cuando llegues", "tell me|when you arrive");
    HAS("avisame cuando llegues porfa", "let me know|when you arrive|please");
    HAS("re lindo", "really nice");
    HAS("el chabón es re piola", "guy|is really cool");
    HAS("la birra está fría", "beer|is cold");
    HAS("qué quilombo", "mess");
    HAS("mañana laburo temprano", "tomorrow|work|early");
    HAS("tomé el bondi", "took|bus");
    HAS("el laburo me tiene re cansado", "job|really tired");
    HAS("Hablá más despacio por favor", "talk|slowly|please");
    LACKS("Hablá más despacio por favor", "he talks");
}

/* ---------------------------------------------------- verbs and tense */

LS_CASE(person_tense_and_aspect)
{
    HAS("Estoy llegando", "almost there");
    HAS("estamos saliendo", "we are going out");
    HAS("estaba lloviendo", "it was raining");
    HAS("Voy a llegar a las 3", "I'm going to arrive|at 3");
    HAS("va a llover", "going to rain");
    HAS("Mañana va a hacer frío", "tomorrow|going to be cold");
    HAS("mañana vendré", "will come");
    HAS("podría ir mañana", "could go");
    HAS("tendremos que esperar", "we will have to wait");
    HAS("hay que esperar", "have to wait");
    HAS("habíamos llegado", "we had arrived");
    HAS("has visto mi mensaje?", "have you seen|message");
    HAS("Fui al centro", "I went");
    HAS("tomé el bondi", "took");
    HAS("Acabo de llegar", "just arrived");
    HAS("Dejé de fumar", "stopped smoking");
    HAS("Sigue lloviendo", "keeps raining");
    HAS("Empezó a llover", "started to rain");
    HAS("Tengo frío", "I am cold");
    HAS("Hace mucho calor hoy", "it is very hot|today");
    HAS("Tengo 30 años", "30 years old");
    HAS("¿Cuántos años tenés?", "how old are you");
    HAS("vamos", "let's go");
    HAS("tengo que irme", "I have to go");
}

LS_CASE(negation_and_questions)
{
    HAS("No tengo batería", "I don't have|battery");
    HAS("no puedo ir hoy", "I can't go|today");
    HAS("no tengo nada", "don't have anything");
    HAS("nadie vino", "nobody came");
    HAS("Ya no funciona", "doesn't work anymore");
    HAS("el bondi no anda", "bus doesn't");
    HAS("¿Podés repetir?", "can you repeat");
    HAS("¿Querés venir?", "do you want to come");
    HAS("¿Qué querés?", "what do you want");
    HAS("¿Dónde está la radio?", "where is the radio");
    HAS("alguien me copia?", "does anyone copy me");
    HAS("Hay alguien en la red?", "anyone");
    HAS("no se que hacer", "don't know what to do");
}

LS_CASE(objects_clitics_and_gustar)
{
    HAS("Te quiero", "I love you");
    HAS("me escucha?", "hear me");
    HAS("quiero verte", "want to see you");
    HAS("estoy esperándote", "waiting for you");
    HAS("Llamame cuando puedas", "call me|when you can");
    HAS("Mandame tu ubicación", "send me your location");
    HAS("Dame un minuto", "give me a minute");
    HAS("Le di el cargador a Pedro", "gave the charger to Pedro");
    HAS("me gusta el mate", "I like the mate");
    HAS("A mí me gusta mucho", "I like it");
    HAS("Me encanta tu antena", "I love your antenna");
    HAS("No me importa", "I don't care");
    HAS("Me queda poca batería", "I have little battery left");
    HAS("Lo vi ayer", "I saw|yesterday");
    HAS("te llega mi mensaje?", "is my message reaching you");
}

LS_CASE(nouns_adjectives_and_order)
{
    HAS("la casa blanca", "the white house");
    HAS("las antenas rotas", "the broken antennas");
    HAS("los nodos nuevos", "the new nodes");
    HAS("Compré una antena nueva", "bought a new antenna");
    HAS("la casa de Juan", "Juan's house");
    HAS("el perro de mi vecino", "my neighbo|dog");
    HAS("Tengo dos perros y un gato", "two dogs|a cat");
    HAS("Las casas son grandes", "the houses are big");
    HAS("Es más grande que el mío", "bigger than mine");
    HAS("Es la mejor antena", "the best antenna");
    HAS("Qué lindo día!", "what a nice day");
    HAS("Vine del cerro", "came from the hill");
    HAS("Se cortó la luz en todo el barrio", "power went out|all over the neighbo");
    HAS("hay un herido en el cerro", "hurt|hill");
    HAS("Necesitamos agua y comida", "we need water and food");
    /* a clause that is a noun and its adjective is a label, not a verb */
    EXACT("Prueba final, me escuchas desde la plaza?", "Final test, can you hear me from the square?");
    HAS("Trabajo nuevo!", "New job");
    HAS("Prueba la antena nueva", "tests the new antenna");   /* with an object it stays a verb */
}

LS_CASE(numbers_names_and_emoji_pass_through)
{
    HAS("Llego a las 9:30", "arrive at 9:30");
    HAS("Son 15 km hasta el cerro", "15 km|to the hill");
    HAS("Nodo LS01 en línea", "node LS01 online");
    HAS("Hola 👋 cómo estás?", "hi 👋 how are you");
    HAS("Saludos desde Córdoba 🙂", "regards from Córdoba 🙂");
    HAS("mi Heltec V3 está cargando", "my Heltec V3 is charging");
    HAS("compré un T-Beam", "bought a T-Beam");
    EXACT("SOS", "SOS");
    EXACT("OK", "OK");
    EXACT("123 456", "123 456");
    HAS("AYUDA NECESITAMOS AGUA", "HELP WE NEED WATER");
    HAS("Juan viene mañana", "Juan comes tomorrow");
}

/* --------------------------------------- lines seen on the screen (round 2) */

LS_CASE(group_questions_and_statement_halves)
{
    /* a third-plural question with no subject is to the group; the statement before it stays a statement */
    EXACT("Mañana a las 10 nos juntamos en la plaza, ¿vienen?",
          "Tomorrow at 10 we meet up in the square, are you coming?");
    EXACT("Mañana a las 10 nos juntamos en la plaza, vienen?",
          "Tomorrow at 10 we meet up in the square, are you coming?");
    HAS("¿Vienen mañana?", "are you coming tomorrow");
    HAS("¿Quieren venir?", "do you want to come");
    HAS("¿Pueden escucharme?", "can you hear me");
    HAS("¿Están todos bien?", "are you all fine");
    HAS("¿Cómo configuraron el canal secundario?", "how did you set up|secondary channel");
    HAS("¿Me pueden mandar la ubicación del punto de encuentro?", "can you send me|meeting point");
    HAS("Hola ¿cómo andás?", "how are you doing");
    /* a question after "si" or "que" inside the question is not turned round */
    HAS("¿Alguien sabe si hay luz en Flores?", "does anyone know if there is power");
    LACKS("¿Alguien sabe si hay luz en Flores?", "is there");
    /* plural commands */
    HAS("Hubo un accidente en la General Paz, eviten la zona", "there was an accident|General Paz|avoid the area");
}

LS_CASE(noun_subjects_and_noun_compounds)
{
    EXACT("El repetidor del cerro vuelve a funcionar", "The hill repeater is working again");
    LACKS("El repetidor del cerro vuelve a funcionar", "it work");
    HAS("El nodo de la plaza no anda", "square node doesn't work");
    LACKS("El nodo de la plaza no anda", " it ");
    HAS("¿Me pasás la clave del canal?", "channel password");
    HAS("Están arreglando la antena del edificio", "fixing the building antenna");
    HAS("¿A qué hora es el encuentro del sábado?", "what time is the Saturday meeting");
    HAS("la casa del vecino", "the neighbo|r's house");
    HAS("Gracias a todos por la ayuda de hoy", "today's help");
    HAS("Llevo la antena y un par de cargadores", "a pair of chargers");
    HAS("Se me terminó el crédito del celular", "cell phone credit|on me");
}

LS_CASE(verbs_with_objects_and_experiencers)
{
    EXACT("Hay que bajar la potencia", "We have to lower the power");
    EXACT("Bajá la potencia", "Lower the power");
    HAS("Subí el archivo", "upload");
    EXACT("Le falta batería al nodo", "The node is low on battery");
    HAS("Le falta batería al nodo de la plaza", "square node is low on battery");
    HAS("Cambié la antena y ahora llego mucho mejor", "come through much better");
    HAS("Mi batería está al 20%, la cargo y vuelvo", "at 20%|charge it and come back");
    LACKS("Mi batería está al 20%, la cargo y vuelvo", "and I come");
}

LS_CASE(your_saturday_and_adjective_first)
{
    EXACT("¿Cuál es su nombre?", "Which is your name?");
    HAS("¿Tiene su radio encendida?", "do you have your radio on");
    EXACT("Nos vemos el sábado", "See you Saturday");
    EXACT("El sábado vamos al cerro", "On Saturday we go to the hill");
    HAS("Si llueve lo pasamos al domingo", "to Sunday");
    HAS("Ojalá que no llueva el sábado", "hopefully it doesn't rain on Saturday");
    EXACT("Re piola tu nodo", "Your node is really cool");
    EXACT("Re lindo el día", "The day is really nice");
    EXACT("Muy buena la señal", "The signal is very good");
    EXACT("Buena pregunta", "Good question");       /* adjective + noun stays as it is */
}

LS_CASE(directions_weather_and_emergencies)
{
    EXACT("Doblá a la derecha en la esquina y seguí dos cuadras", "Turn right on the corner and continue two blocks");
    EXACT("Estoy a tres cuadras de la estación", "I am three blocks from the station");
    EXACT("Vivo a dos cuadras", "I live two blocks away");
    HAS("Estoy a 3 km del cerro", "3 km from the hill");
    HAS("Nos juntamos a las 18 en Plaza Italia, frente al monumento", "at 18|Plaza Italia|facing the monument");
    HAS("El acceso está por la calle de atrás", "along the street");
    HAS("Se inundó la calle, no se puede pasar", "the street flooded|you can't pass");
    HAS("Hay un corte de luz en todo Palermo", "a power outage all over Palermo");
    HAS("Ojo que en zona oeste está cayendo granizo", "careful|west|hail is falling");
    HAS("Mi viejo se descompuso, ¿alguien tiene auto?", "got sick|does anyone have");
    HAS("Perdón la demora, se me cortó el bondi", "sorry for the delay|bus cut out on me");
    HAS("Prendé el GPS así te vemos", "turn on the GPS so we see you");
}

/* ------------------------------------------------------------ detection */

static mt_lang_t det(const char *s, int *c) { return mt_xlate_detect(s, c); }

LS_CASE(detects_spanish)
{
    static const char *const es[] = {
        "hola", "gracias", "hola che", "todo bien?", "estoy en la plaza", "no tengo batería", "dnd estas",
        "q onda", "te copio", "me escuchas?", "mañana subo al cerro", "dale nos vemos", "se cortó la luz",
        "Necesito ayuda", "voy para alla", "buen dia", "¿Dónde estás?", "jajaja buenisimo che",
    };
    unsigned k;
    for (k = 0; k < sizeof es / sizeof es[0]; k++) {
        int c = -1;
        mt_lang_t l = det(es[k], &c);
        LS_CHECK_MSG(l == MT_LANG_ES, "[%s] read as %d (%d%%)", es[k], l, c);
        LS_CHECK_MSG(c > 0 && c <= 100, "[%s] confidence %d", es[k], c);
    }
}

LS_CASE(detects_english_and_does_not_call_it_spanish)
{
    static const char *const en[] = {
        "hello", "thanks!", "see you", "hi there", "Where are you?", "I'm at the plaza", "No signal here",
        "Come here", "me too", "Copy that", "Let's meet at the square", "Can you hear me?",
        "My battery is low", "The radio is on the table", "I need help", "I'm at Plaza de Mayo now",
        "going to the base camp", "what's up", "lol ok", "Is anyone on the mesh tonight?",
    };
    unsigned k;
    for (k = 0; k < sizeof en / sizeof en[0]; k++) {
        int c = -1;
        mt_lang_t l = det(en[k], &c);
        LS_CHECK_MSG(l != MT_LANG_ES, "[%s] read as Spanish (%d%%)", en[k], c);
        LS_CHECK_MSG(l == MT_LANG_EN || l == MT_LANG_UNKNOWN, "[%s] read as %d", en[k], l);
    }
    /* the longer ones are English for sure */
    LS_EQ_INT(det("Where are you?", 0), MT_LANG_EN);
    LS_EQ_INT(det("My battery is low", 0), MT_LANG_EN);
    LS_EQ_INT(det("Is anyone on the mesh tonight?", 0), MT_LANG_EN);
}

LS_CASE(detects_unknown_and_mixed)
{
    int c = 5;
    LS_EQ_INT(det("ok", &c), MT_LANG_UNKNOWN);
    LS_EQ_INT(c, 0);
    LS_EQ_INT(det("OK", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det("123", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det("12:30 -34.6 -58.4", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det("", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det(NULL, 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det("👍👍", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det("LS01", 0), MT_LANG_UNKNOWN);
    LS_EQ_INT(det("no", 0), MT_LANG_UNKNOWN);
    /* mixed: the language with most of the words wins */
    LS_EQ_INT(det("ok dale, nos vemos mañana en la plaza", 0), MT_LANG_ES);
    LS_EQ_INT(det("hola! how are you doing today?", 0), MT_LANG_EN);
    LS_EQ_INT(det("che, the antenna is broken again", 0), MT_LANG_EN);
}

LS_CASE(incoming_translates_only_spanish)
{
    char b[256];
    LS_CHECK(mt_xlate_incoming("estoy en la plaza", b, sizeof b) > 0);
    LS_EQ_STR(b, "I am in the square");
    LS_EQ_INT(mt_xlate_incoming("I'm at the plaza", b, sizeof b), 0);
    LS_EQ_STR(b, "");
    LS_EQ_INT(mt_xlate_incoming("ok", b, sizeof b), 0);
    LS_EQ_INT(mt_xlate_incoming("estoy en la plaza", b, 0), 0);
}

/* ------------------------------------------------------------- limits */

LS_CASE(empty_null_and_tiny_buffers)
{
    char b[64];
    unsigned cap, k;
    LS_EQ_INT(mt_xlate_es_en("", b, sizeof b), 0);
    LS_EQ_STR(b, "");
    LS_EQ_INT(mt_xlate_es_en(NULL, b, sizeof b), 0);
    LS_EQ_STR(b, "");
    LS_EQ_INT(mt_xlate_es_en("hola", NULL, 10), 0);
    b[0] = 'x';
    LS_EQ_INT(mt_xlate_es_en("hola", b, 0), 0);
    LS_EQ_INT(b[0], 'x');                       /* cap 0 writes nothing */
    /* every small cap: NUL-terminated, never past cap, the length returned */
    for (cap = 1; cap < 48; cap++) {
        char g[80];
        int n;
        memset(g, 0x5A, sizeof g);
        n = mt_xlate_es_en("Hola che, ¿cómo andás? Mañana subo al cerro con la radio 👋", g, cap);
        LS_CHECK_MSG(n >= 0 && (unsigned)n < cap, "cap %u returned %d", cap, n);
        LS_CHECK_MSG(g[n] == 0 && strlen(g) == (size_t)n, "cap %u not terminated", cap);
        for (k = cap; k < sizeof g; k++) LS_CHECK_MSG(g[k] == 0x5A, "cap %u wrote byte %u", cap, k);
    }
}

LS_CASE(long_and_strange_input)
{
    static char big[4096], res[8192];
    int n, k;
    const char *line = "hola che, todo bien? mañana subo al cerro con la radio y la antena nueva. ";
    big[0] = 0;
    while (strlen(big) + strlen(line) < sizeof big - 1) strcat(big, line);
    n = mt_xlate_es_en(big, res, sizeof res);
    LS_CHECK(n > 1000 && (size_t)n == strlen(res));
    LS_CHECK(has_all(res, "tomorrow|hill|radio|new antenna"));
    /* 200+ characters in one message, as on the mesh */
    n = mt_xlate_es_en("Buenas noches a todos los nodos de Buenos Aires, ¿alguien me copia? Estoy en Palermo "
                       "cerca de la estación con poca batería, mañana subo al cerro con la radio nueva y les aviso "
                       "cómo llega la señal desde ahí. Saludos!", res, sizeof res);
    LS_CHECK(n > 100);
    LS_CHECK(has_all(res, "good night|all the nodes|anyone copy me|palermo|station|tomorrow|regards"));
    /* bytes that are not UTF-8 text, control characters, lone continuation bytes */
    {
        static const char junk[] = "\xff\xfe hola \x80\x81 \xc3 che \x01\x02\x7f \xe2\x82 ok \xf0\x9f";
        n = mt_xlate_es_en(junk, res, sizeof res);
        LS_CHECK(n >= 0 && (size_t)n == strlen(res));
        LS_CHECK(has_all(res, "hi|hey"));
    }
    /* accents, n-tilde, inverted marks: kept or dropped, never broken */
    n = mt_xlate_es_en("¿Año? ¡Señal! Ñandú, pingüino, acción", res, sizeof res);
    LS_CHECK(n > 0);
    LS_CHECK(strstr(res, "\xc2\xbf") == NULL && strstr(res, "\xc2\xa1") == NULL);
    LS_CHECK(has_all(res, "year?|signal!"));
    /* a NUL ends the message */
    {
        static const char withnul[] = "hola\0che";
        mt_xlate_es_en(withnul, res, sizeof res);
        LS_EQ_STR(res, "hi");
    }
    /* one very long word passes through */
    {
        char w[300];
        memset(w, 'a', sizeof w - 1);
        w[sizeof w - 1] = 0;
        n = mt_xlate_es_en(w, res, sizeof res);
        LS_EQ_INT(n, (int)strlen(w));
    }
    /* only punctuation, only spaces */
    LS_EQ_STR(tr("?!..."), "?!...");
    LS_EQ_STR(tr("    "), "");
    for (k = 0; k < 3; k++) LS_CHECK(mt_xlate_es_en("q", res, sizeof res) >= 0);
}

LS_CASE(fold_ascii_for_the_screen)
{
    char s[64];
    strcpy(s, "¿Dónde estás? Año, ÑANDÚ 👋 ok");
    LS_EQ_INT(mt_xlate_fold_ascii(s), (int)strlen("Donde estas? Ano, NANDU ? ok"));
    LS_EQ_STR(s, "Donde estas? Ano, NANDU ? ok");
    strcpy(s, "\xff\xfe" "abc");
    mt_xlate_fold_ascii(s);
    LS_EQ_STR(s, "?abc");
    LS_EQ_INT(mt_xlate_fold_ascii(NULL), 0);
}

/* ------------------------------------------------------------ phrasebook */

LS_CASE(phrasebook)
{
    int n = mt_phrase_count(), i, c, cats = mt_phrase_category_count(), back = 0;
    LS_CHECK(n >= 40 && n <= 60);
    LS_CHECK(cats >= 5);
    for (i = 0; i < n; i++) {
        mt_phrase_t p = mt_phrase(i);
        int known = 0;
        LS_CHECK(p.en[0] && p.es[0] && p.category[0]);
        for (c = 0; c < cats; c++) if (!strcmp(p.category, mt_phrase_category(c))) known = 1;
        LS_CHECK(known);
        /* the Spanish to send reads back as something close to the English */
        mt_xlate_es_en(p.es, out, sizeof out);
        {
            char w[32];
            const char *e = p.en;
            int found = 0;
            /* any word of four letters or more of the English line */
            while (*e) {
                int m = 0;
                while (e[m] && isalpha((unsigned char)e[m])) m++;
                if (m >= 4) {
                    snprintf(w, sizeof w, "%.*s", m, e);
                    if (has_all(out, w)) { found = 1; break; }
                }
                e += m ? m : 1;
            }
            back += found || strlen(p.en) < 6;
        }
    }
    LS_CHECK_MSG(back * 10 >= n * 7, "only %d of %d phrasebook lines read back", back, n);
    LS_EQ_STR(mt_phrase(-1).en, "");
    LS_EQ_STR(mt_phrase(n).es, "");
    LS_EQ_STR(mt_phrase_category(cats), "");
    {
        int found = 0;
        for (i = 0; i < n; i++) if (!strcmp(mt_phrase(i).en, "Where are you?")) found = 1;
        LS_CHECK(found);
    }
}

/* ---------------------------------------------------------- performance */

LS_CASE(cost_of_a_full_message)
{
    static const char msg[] = "Buenas noches a todos, ¿alguien me copia? Estoy en Palermo cerca de la estación con "
                              "poca batería, mañana subo al cerro con la radio y les aviso cómo llega la señal. "
                              "Saludos y gracias!";
    clock_t t0 = clock();
    int k;
    double us;
    for (k = 0; k < 500; k++) mt_xlate_es_en(msg, out, sizeof out);
    us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 500;
    ls_note("translate %d bytes: %.0f us per message on this host", (int)strlen(msg), us);
    LS_CHECK_MSG(us < 5000, "%.0f us per message", us);
}

/* ---------------------------------------------------- side by side table */

LS_CASE(side_by_side)
{
    static const char *const s[] = {
        "Hola che, ¿cómo andás?", "Buenas noches a todos los nodos", "¿Alguien me copia?",
        "Te escucho con poca señal", "Estoy en Palermo, cerca de la estación", "Mi nodo está en la terraza",
        "Mañana subo al cerro con la radio", "¿Tenés buena señal desde ahí?", "Se viene una tormenta fuerte",
        "Está lloviendo mucho acá", "Se cortó la luz en todo el barrio", "No tengo batería, me desconecto",
        "Me queda poca batería", "Voy a llegar a las 9:30", "Nos vemos a las 8 en la esquina",
        "Esperame en la puerta", "Avisame cuando llegues porfa", "Decime dónde estás",
        "xq no contestás?", "q onda boludo, todo bien?", "Re piola tu nodo", "El bondi no anda",
        "Mañana laburo temprano", "Vamos a tomar unas birras", "Hay un herido en el cerro",
        "Necesitamos agua y comida", "Llamen a una ambulancia", "Estoy perdido",
        "Ya llegué", "Acabo de llegar", "¿Qué hora es?", "Son las 5 de la tarde",
        "Hace mucho frío hoy", "Mañana va a llover", "Tengo que irme", "No me importa",
        "¿Querés venir?", "Puedo ayudarte", "La casa de Juan", "El perro de mi vecino",
        "Compré una antena nueva", "Es más grande que el mío", "Qué lindo día!",
    };
    unsigned k;
    printf("\n%-42s  %s\n", "Spanish", "English");
    for (k = 0; k < sizeof s / sizeof s[0]; k++) {
        int c;
        mt_lang_t l = mt_xlate_detect(s[k], &c);
        mt_xlate_es_en(s[k], out, sizeof out);
        printf("%-42s  %s   [%s %d%%]\n", s[k], out, l == MT_LANG_ES ? "es" : l == MT_LANG_EN ? "en" : "??", c);
        LS_CHECK(out[0]);
    }
}
